/*
 * Copyright 2026, RenkuOS Contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


//!	Driver for the SPI keyboard and trackpad of the MacBook (2015-2017) and
//	MacBook Pro (2016-2017), ACPI hardware ID APP000D.
//
//	The keyboard is published as input/keyboard/spi/N and handled by the
//	keyboard code shared with the USB and I2C HID drivers. The trackpad is
//	published as input/mouse/spi/N and speaks the touchpad protocol, so that
//	input_server's touchpad settings and gestures apply to it.


#include <ACPI.h>
#include <device_manager.h>
#include <spi.h>

#include <lock.h>
#include <util/AutoLock.h>

#include <new>
#include <stdio.h>
#include <string.h>

#include "AppleSPIDevice.h"
#include "AppleSPITouchpad.h"
#include "DeviceList.h"
#include "Driver.h"
#include "HIDDevice.h"
#include "ProtocolHandler.h"


#define APPLE_SPI_DRIVER_NAME "drivers/input/apple_spi/driver_v1"
#define APPLE_SPI_DEVICE_NAME "drivers/input/apple_spi/device_v1"

#define APPLE_SPI_HID "APP000D"


struct apple_spi_cookie {
	AppleSPIDevice*		device;
	ProtocolHandler*	handler;	// the keyboard, or NULL
	AppleSPITouchpad*	touchpad;	// the trackpad, or NULL
	uint32				handlerCookie;
};


static device_manager_info* sDeviceManager;
static acpi_module_info* sACPI;

DeviceList* gDeviceList = NULL;
static mutex sDriverLock;


// #pragma mark - device hooks


static status_t
apple_spi_init_device(void* driverCookie, void** cookie)
{
	*cookie = driverCookie;
	return B_OK;
}


static void
apple_spi_uninit_device(void* cookie)
{
}


static status_t
apple_spi_open(void* initCookie, const char* path, int flags, void** _cookie)
{
	TRACE("open(%s, %d)\n", path, flags);

	apple_spi_cookie* cookie = new(std::nothrow) apple_spi_cookie();
	if (cookie == NULL)
		return B_NO_MEMORY;

	MutexLocker locker(sDriverLock);

	AppleSPIDevice* device = (AppleSPIDevice*)initCookie;
	void* entry = gDeviceList->FindDevice(path);
	if (device == NULL || entry == NULL || device->IsRemoved()) {
		delete cookie;
		return B_ENTRY_NOT_FOUND;
	}

	cookie->device = device;
	status_t status;
	if (entry == device->Touchpad()) {
		cookie->touchpad = device->Touchpad();
		status = cookie->touchpad->Open(flags);
	} else {
		cookie->handler = (ProtocolHandler*)entry;
		status = cookie->handler->Open(flags, &cookie->handlerCookie);
	}

	if (status != B_OK) {
		delete cookie;
		return status;
	}

	device->AcquireReference();
	*_cookie = cookie;
	return B_OK;
}


static status_t
apple_spi_read(void* _cookie, off_t position, void* buffer, size_t* numBytes)
{
	apple_spi_cookie* cookie = (apple_spi_cookie*)_cookie;
	if (cookie->handler == NULL) {
		*numBytes = 0;
		return B_ERROR;
	}
	return cookie->handler->Read(&cookie->handlerCookie, position, buffer,
		numBytes);
}


static status_t
apple_spi_write(void* _cookie, off_t position, const void* buffer,
	size_t* numBytes)
{
	apple_spi_cookie* cookie = (apple_spi_cookie*)_cookie;
	if (cookie->handler == NULL) {
		*numBytes = 0;
		return B_ERROR;
	}
	return cookie->handler->Write(&cookie->handlerCookie, position, buffer,
		numBytes);
}


static status_t
apple_spi_control(void* _cookie, uint32 op, void* buffer, size_t length)
{
	apple_spi_cookie* cookie = (apple_spi_cookie*)_cookie;
	if (cookie->touchpad != NULL)
		return cookie->touchpad->Control(op, buffer, length);
	return cookie->handler->Control(&cookie->handlerCookie, op, buffer, length);
}


static status_t
apple_spi_close(void* _cookie)
{
	apple_spi_cookie* cookie = (apple_spi_cookie*)_cookie;
	if (cookie->touchpad != NULL)
		return cookie->touchpad->Close();
	return cookie->handler->Close(&cookie->handlerCookie);
}


static status_t
apple_spi_free(void* _cookie)
{
	apple_spi_cookie* cookie = (apple_spi_cookie*)_cookie;

	MutexLocker locker(sDriverLock);
	if (cookie->device->ReleaseReference()) {
		// removed, and this was the last open descriptor
		delete cookie->device;
	}
	locker.Unlock();

	delete cookie;
	return B_OK;
}


//	#pragma mark - driver module


static bool
is_apple_spi_name(const char* name)
{
	return name != NULL && strcmp(name, APPLE_SPI_HID) == 0;
}


static float
apple_spi_support(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK) {
		return -1;
	}
	if (strcmp(bus, "spi") != 0)
		return 0.0f;

	const char* name;
	if (sDeviceManager->get_attr_string(parent, ACPI_DEVICE_HID_ITEM, &name,
			false) == B_OK && is_apple_spi_name(name)) {
		return 0.6f;
	}
	if (sDeviceManager->get_attr_string(parent, ACPI_DEVICE_CID_ITEM, &name,
			false) == B_OK && is_apple_spi_name(name)) {
		return 0.6f;
	}

	return 0.0f;
}


static status_t
apple_spi_register_device(device_node* node)
{
	CALLED();

	// Stay initialized: the reader thread uses the SPI bus below us, which
	// the device manager would otherwise shut down whenever nothing has the
	// keyboard open.
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "Apple SPI keyboard and trackpad" }},
		{ B_DEVICE_FLAGS, B_UINT32_TYPE, { .ui32 = B_KEEP_DRIVER_LOADED }},
		{ NULL }
	};

	return sDeviceManager->register_node(node, APPLE_SPI_DRIVER_NAME, attrs,
		NULL, NULL);
}


static status_t
apple_spi_init_driver(device_node* node, void** driverCookie)
{
	CALLED();

	spi_device_interface* spi;
	spi_device spiCookie;
	uint64 handle;

	device_node* parent = sDeviceManager->get_parent_node(node);
	sDeviceManager->get_driver(parent, (driver_module_info**)&spi,
		(void**)&spiCookie);
	status_t status = sDeviceManager->get_attr_uint64(parent,
		ACPI_DEVICE_HANDLE_ITEM, &handle, false);
	sDeviceManager->put_node(parent);
	if (status != B_OK || handle == 0)
		return B_DEV_CONFIGURATION_ERROR;

	MutexLocker locker(sDriverLock);

	AppleSPIDevice* device = new(std::nothrow) AppleSPIDevice(node, spi,
		spiCookie, sACPI, (acpi_handle)(addr_t)handle);
	if (device == NULL)
		return B_NO_MEMORY;

	status = device->InitCheck();
	if (status != B_OK) {
		delete device;
		return status;
	}

	*driverCookie = device;
	return B_OK;
}


static void
apple_spi_uninit_driver(void* driverCookie)
{
	CALLED();
	AppleSPIDevice* device = (AppleSPIDevice*)driverCookie;

	MutexLocker locker(sDriverLock);

	// Only happens when the device goes away: the node keeps itself loaded.
	for (uint32 i = 0;; i++) {
		ProtocolHandler* handler = device->Keyboard()->ProtocolHandlerAt(i);
		if (handler == NULL)
			break;
		gDeviceList->RemoveDevice(NULL, handler);
	}
	gDeviceList->RemoveDevice(NULL, device->Touchpad());

	device->Removed();
	device->AcquireReference();
	if (device->ReleaseReference())
		delete device;
}


static status_t
publish(AppleSPIDevice* device, const char* basePath, void* entry,
	char** _path)
{
	// The device may have siblings from another driver under the same
	// base path; use the first free name.
	char path[B_DEV_NAME_LENGTH];
	for (int32 index = 0;; index++) {
		snprintf(path, sizeof(path), "%s%" B_PRId32, basePath, index);
		if (gDeviceList->FindDevice(path) == NULL)
			break;
	}

	*_path = strdup(path);
	if (*_path == NULL)
		return B_NO_MEMORY;

	gDeviceList->AddDevice(*_path, entry);
	return sDeviceManager->publish_device(device->Node(), *_path,
		APPLE_SPI_DEVICE_NAME);
}


static status_t
apple_spi_register_child_devices(void* driverCookie)
{
	CALLED();
	AppleSPIDevice* device = (AppleSPIDevice*)driverCookie;

	MutexLocker locker(sDriverLock);
	if (device->IsPublished())
		return B_OK;

	for (uint32 i = 0;; i++) {
		ProtocolHandler* handler = device->Keyboard()->ProtocolHandlerAt(i);
		if (handler == NULL)
			break;

		char* path;
		status_t status = publish(device, handler->BasePath(), handler, &path);
		if (path != NULL)
			handler->SetPublishPath(path);
		if (status != B_OK)
			ERROR("publishing %s failed: %s\n", path, strerror(status));
	}

	char* path;
	status_t status = publish(device, "input/mouse/" DEVICE_PATH_SUFFIX "/",
		device->Touchpad(), &path);
	if (path != NULL)
		device->Touchpad()->SetPublishPath(path);
	if (status != B_OK)
		ERROR("publishing %s failed: %s\n", path, strerror(status));

	device->SetPublished();
	return B_OK;
}


static status_t
std_ops(int32 op, ...)
{
	switch (op) {
		case B_MODULE_INIT:
			gDeviceList = new(std::nothrow) DeviceList();
			if (gDeviceList == NULL)
				return B_NO_MEMORY;
			mutex_init(&sDriverLock, "apple_spi driver lock");
			return B_OK;

		case B_MODULE_UNINIT:
			delete gDeviceList;
			gDeviceList = NULL;
			mutex_destroy(&sDriverLock);
			return B_OK;

		default:
			break;
	}

	return B_ERROR;
}


driver_module_info apple_spi_driver_module = {
	{
		APPLE_SPI_DRIVER_NAME,
		0,
		&std_ops
	},

	apple_spi_support,
	apple_spi_register_device,
	apple_spi_init_driver,
	apple_spi_uninit_driver,
	apple_spi_register_child_devices,
	NULL,	// rescan
	NULL,	// removed
};


struct device_module_info apple_spi_device_module = {
	{
		APPLE_SPI_DEVICE_NAME,
		0,
		NULL
	},

	apple_spi_init_device,
	apple_spi_uninit_device,
	NULL,

	apple_spi_open,
	apple_spi_close,
	apple_spi_free,
	apple_spi_read,
	apple_spi_write,
	NULL,
	apple_spi_control,

	NULL,
	NULL
};


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{ B_ACPI_MODULE_NAME, (module_info**)&sACPI },
	{}
};


module_info* modules[] = {
	(module_info*)&apple_spi_driver_module,
	(module_info*)&apple_spi_device_module,
	NULL
};
