/*
 * Copyright 2026, RenkuOS Contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


//!	Driver for the FaceTime HD camera of the MacBook Pro (2015-2017).
//	It brings the camera's chip and memory up, starts Apple's firmware on
//	it, and streams 1280x720 YUYV frames through a private ioctl interface
//	(facetimehd_ioctl.h).


#include "facetimehd.h"
#include "facetimehd_ioctl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Drivers.h>
#include <kernel.h>
#include <lock.h>
#include <util/AutoLock.h>


#define DEVICE_NAME		"facetimehd"
#define MAX_DEVICES		1


int32 api_version = B_CUR_DRIVER_API_VERSION;

pci_module_info* gPCI;

static fthd_device sDevices[MAX_DEVICES];
static char* sDeviceNames[MAX_DEVICES + 1];
static int32 sDeviceCount;
static mutex sLock = MUTEX_INITIALIZER("facetimehd");

// Without a stream for this long, the processor and sensor are switched
// off; the next stream switches them on again (about half a second).
static const bigtime_t FTHD_IDLE_DELAY = 15000000;


/*!	Checks once a second whether the camera has been unused for
	FTHD_IDLE_DELAY and powers it down if so.
*/
static status_t
power_thread(void* data)
{
	fthd_device* device = (fthd_device*)data;
	while (!device->power_thread_quit) {
		acquire_sem_etc(device->power_sem, 1, B_RELATIVE_TIMEOUT, 1000000);
		if (device->power_thread_quit)
			break;

		MutexLocker locker(device->stream_lock);
		if (device->isp_up && !device->streaming
			&& system_time() - device->last_used >= FTHD_IDLE_DELAY) {
			fthd_power_down(device);
		}
	}
	return B_OK;
}


static void
start_power_thread(fthd_device* device)
{
	device->power_thread = -1;
	device->power_thread_quit = false;
	device->last_used = system_time();
	device->power_sem = create_sem(0, "facetimehd power");
	if (device->power_sem < 0)
		return;

	device->power_thread = spawn_kernel_thread(power_thread,
		"facetimehd power", B_LOW_PRIORITY, device);
	if (device->power_thread < 0) {
		delete_sem(device->power_sem);
		device->power_sem = -1;
		FTHD_LOG("no power thread; the camera stays on\n");
		return;
	}
	resume_thread(device->power_thread);
}


static void
stop_power_thread(fthd_device* device)
{
	if (device->power_thread >= 0) {
		device->power_thread_quit = true;
		release_sem(device->power_sem);
		status_t result;
		wait_for_thread(device->power_thread, &result);
		device->power_thread = -1;
	}
	if (device->power_sem >= 0) {
		delete_sem(device->power_sem);
		device->power_sem = -1;
	}
}


static bool
is_camera(const pci_info& info)
{
	return info.vendor_id == FTHD_VENDOR_BROADCOM
		&& info.device_id == FTHD_DEVICE_1570;
}


// #pragma mark - device hooks


struct fthd_cookie {
	fthd_device*	device;
	bool			started;	// this opener started the stream
};


static status_t
fthd_open(const char* name, uint32 flags, void** _cookie)
{
	for (int32 i = 0; i < sDeviceCount; i++) {
		if (strcmp(name, sDeviceNames[i]) != 0)
			continue;

		fthd_cookie* cookie = (fthd_cookie*)malloc(sizeof(fthd_cookie));
		if (cookie == NULL)
			return B_NO_MEMORY;
		cookie->device = &sDevices[i];
		cookie->started = false;
		*_cookie = cookie;
		return B_OK;
	}
	return B_ENTRY_NOT_FOUND;
}


static status_t
fthd_close(void* _cookie)
{
	fthd_cookie* cookie = (fthd_cookie*)_cookie;
	if (cookie->started) {
		fthd_stream_stop(cookie->device);
		cookie->started = false;
	}
	return B_OK;
}


static status_t
fthd_free(void* cookie)
{
	free(cookie);
	return B_OK;
}


static status_t
fthd_ioctl(void* _cookie, uint32 op, void* buffer, size_t length)
{
	fthd_cookie* cookie = (fthd_cookie*)_cookie;
	fthd_device* device = cookie->device;

	switch (op) {
		case FTHD_GET_INFO:
		{
			if (buffer == NULL || length < sizeof(fthd_info)
				|| !IS_USER_ADDRESS(buffer)) {
				return B_BAD_VALUE;
			}

			fthd_info info = {};
			info.version = FTHD_IOCTL_VERSION;
			info.sensor_id0 = device->sensor_id0;
			info.sensor_id1 = device->sensor_id1;
			info.sensor_count = device->sensor_count;
			info.sensor_width = device->sensor_width;
			info.sensor_height = device->sensor_height;
			info.width = device->width;
			info.height = device->height;
			info.bytes_per_row = device->width * 2;
			info.frame_size = device->width * device->height * 2;
			info.streaming = device->streaming;
			info.frames_received = atomic_get(&device->frames_received);
			// also while the camera is powered down
			info.set_file_loaded = device->set_file_seen;
			return user_memcpy(buffer, &info, sizeof(info));
		}

		case FTHD_GET_CONTROL:
		case FTHD_SET_CONTROL:
		{
			fthd_control control;
			if (buffer == NULL || length < sizeof(control)
				|| !IS_USER_ADDRESS(buffer)
				|| user_memcpy(&control, buffer, sizeof(control)) != B_OK
				|| control.id >= FTHD_CONTROL_COUNT) {
				return B_BAD_VALUE;
			}

			if (op == FTHD_SET_CONTROL)
				return fthd_stream_set_control(device, control.id,
					control.value);

			control.value = device->controls[control.id];
			return user_memcpy(buffer, &control, sizeof(control));
		}

		case FTHD_START:
		{
			status_t status = fthd_stream_start(device);
			if (status == B_OK)
				cookie->started = true;
			return status;
		}

		case FTHD_STOP:
			if (!cookie->started)
				return B_NOT_ALLOWED;
			fthd_stream_stop(device);
			cookie->started = false;
			return B_OK;

		case FTHD_READ_FRAME:
		{
			fthd_frame_request request;
			if (buffer == NULL || length < sizeof(request)
				|| !IS_USER_ADDRESS(buffer)
				|| user_memcpy(&request, buffer, sizeof(request)) != B_OK
				|| !IS_USER_ADDRESS(request.buffer)) {
				return B_BAD_VALUE;
			}

			bigtime_t timeout = request.timeout > 0
				? request.timeout : 1000000;
			status_t status = fthd_stream_read_frame(device, request.buffer,
				request.buffer_size, timeout, &request.sequence,
				&request.time);
			if (status != B_OK)
				return status;
			return user_memcpy(buffer, &request, sizeof(request));
		}
	}

	return B_DEV_INVALID_IOCTL;
}


static status_t
fthd_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	*_length = 0;
	return B_OK;
}


static status_t
fthd_write(void* cookie, off_t position, const void* buffer,
	size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static device_hooks sHooks = {
	fthd_open,
	fthd_close,
	fthd_free,
	fthd_ioctl,
	fthd_read,
	fthd_write
};


// #pragma mark - driver


extern "C" status_t
init_hardware(void)
{
	if (get_module(B_PCI_MODULE_NAME, (module_info**)&gPCI) != B_OK)
		return ENODEV;

	status_t status = ENODEV;
	pci_info info;
	for (int32 i = 0; gPCI->get_nth_pci_info(i, &info) == B_OK; i++) {
		if (is_camera(info)) {
			status = B_OK;
			break;
		}
	}

	put_module(B_PCI_MODULE_NAME);
	return status;
}


extern "C" status_t
init_driver(void)
{
	if (get_module(B_PCI_MODULE_NAME, (module_info**)&gPCI) != B_OK)
		return ENODEV;

	MutexLocker locker(sLock);
	sDeviceCount = 0;

	pci_info info;
	for (int32 i = 0; gPCI->get_nth_pci_info(i, &info) == B_OK
			&& sDeviceCount < MAX_DEVICES; i++) {
		if (!is_camera(info))
			continue;

		fthd_device* device = &sDevices[sDeviceCount];
		memset(device, 0, sizeof(*device));
		device->pci = info;

		if (gPCI->reserve_device(info.bus, info.device, info.function,
				DEVICE_NAME, device) != B_OK) {
			FTHD_LOG("PCI %d:%d:%d is in use by another driver\n", info.bus,
				info.device, info.function);
			continue;
		}

		FTHD_LOG("camera at PCI %d:%d:%d, subsystem %04x:%04x\n", info.bus,
			info.device, info.function, info.u.h0.subsystem_vendor_id,
			info.u.h0.subsystem_id);

		if (fthd_map(device) != B_OK) {
			gPCI->unreserve_device(info.bus, info.device, info.function,
				DEVICE_NAME, device);
			continue;
		}

		fthd_memory_init(device);
		fthd_stream_init(device);
		status_t status = fthd_hardware_init(device);
		FTHD_LOG("bring-up %s\n", status == B_OK ? "done"
			: strerror(status));
		if (status == B_OK) {
			// Started once to learn the sensor and its calibration; the
			// power thread switches it off again while it is unused.
			status = fthd_isp_init(device);
			FTHD_LOG("firmware start %s\n", status == B_OK ? "done"
				: strerror(status));
		}
		start_power_thread(device);

		char name[64];
		snprintf(name, sizeof(name), "video/" DEVICE_NAME "/%" B_PRId32,
			sDeviceCount);
		sDeviceNames[sDeviceCount] = strdup(name);
		sDeviceCount++;
	}
	sDeviceNames[sDeviceCount] = NULL;

	if (sDeviceCount == 0) {
		put_module(B_PCI_MODULE_NAME);
		return ENODEV;
	}
	return B_OK;
}


extern "C" void
uninit_driver(void)
{
	MutexLocker locker(sLock);
	for (int32 i = 0; i < sDeviceCount; i++) {
		fthd_device* device = &sDevices[i];
		stop_power_thread(device);
		fthd_stream_stop(device);
		fthd_isp_uninit(device);
		fthd_unmap(device);
		fthd_stream_uninit(device);
		fthd_memory_uninit(device);
		gPCI->unreserve_device(device->pci.bus, device->pci.device,
			device->pci.function, DEVICE_NAME, device);
		free(sDeviceNames[i]);
		sDeviceNames[i] = NULL;
	}
	sDeviceCount = 0;
	put_module(B_PCI_MODULE_NAME);
}


extern "C" const char**
publish_devices(void)
{
	return (const char**)sDeviceNames;
}


extern "C" device_hooks*
find_device(const char* name)
{
	return &sHooks;
}
