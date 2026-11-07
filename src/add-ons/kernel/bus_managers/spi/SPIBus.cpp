/*
 * Copyright 2026, RenkuOS Contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


#include "SPIPrivate.h"


SPIBus::SPIBus(device_node* node, uint8 id)
	:
	fStatus(B_NO_INIT),
	fNode(node),
	fID(id),
	fController(NULL),
	fCookie(NULL)
{
	CALLED();
	device_node* parent = gDeviceManager->get_parent_node(node);
	fStatus = gDeviceManager->get_driver(parent,
		(driver_module_info**)&fController, &fCookie);
	gDeviceManager->put_node(parent);

	if (fStatus != B_OK)
		return;

	if (fController->transfer == NULL) {
		fStatus = B_NOT_SUPPORTED;
		return;
	}

	fController->set_spi_bus(fCookie, this);
}


status_t
SPIBus::Transfer(const spi_device_config* config,
	const spi_transfer* transfers, size_t count)
{
	CALLED();
	status_t status = spi_validate_transfers(config, transfers, count);
	if (status != B_OK)
		return status;

	return fController->transfer(fCookie, config, transfers, count);
}


status_t
SPIBus::RegisterDevice(const spi_device_config* config, const char* hid,
	char** cid, acpi_handle acpiHandle)
{
	CALLED();

	// register_node() copies the attributes, so pointing at the caller's
	// strings is fine. Attributes without a value are left out.
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE, { .string = "SPI device" }},

		// connection
		{ SPI_DEVICE_CHIP_SELECT_ITEM, B_UINT16_TYPE,
			{ .ui16 = config->chip_select }},
		{ SPI_DEVICE_SPEED_ITEM, B_UINT32_TYPE, { .ui32 = config->speed_hz }},
		{ SPI_DEVICE_MODE_ITEM, B_UINT8_TYPE, { .ui8 = config->mode }},
		{ SPI_DEVICE_BITS_ITEM, B_UINT8_TYPE,
			{ .ui8 = config->bits_per_word }},

		// description of peripheral drivers
		{ B_DEVICE_BUS, B_STRING_TYPE, { .string = "spi" }},
		{ B_DEVICE_FLAGS, B_UINT32_TYPE, { .ui32 = B_FIND_MULTIPLE_CHILDREN }},
		{ ACPI_DEVICE_HANDLE_ITEM, B_UINT64_TYPE,
			{ .ui64 = (addr_t)acpiHandle }},

		{ NULL },	// hid
		{ NULL },	// cid
		{ NULL }
	};

	int index = 8;
	if (hid != NULL) {
		attrs[index].name = ACPI_DEVICE_HID_ITEM;
		attrs[index].type = B_STRING_TYPE;
		attrs[index].value.string = hid;
		index++;
	}
	if (cid != NULL && cid[0] != NULL) {
		attrs[index].name = ACPI_DEVICE_CID_ITEM;
		attrs[index].type = B_STRING_TYPE;
		attrs[index].value.string = cid[0];
	}

	TRACE_ALWAYS("bus %u: %s, chip select %u, %" B_PRIu32 " Hz, mode %u\n",
		fID, hid != NULL ? hid : "(no hid)", config->chip_select,
		config->speed_hz, config->mode);

	return gDeviceManager->register_node(fNode, SPI_DEVICE_MODULE_NAME, attrs,
		NULL, NULL);
}


status_t
SPIBus::Scan()
{
	CALLED();
	if (fController->scan_bus != NULL)
		return fController->scan_bus(fCookie);
	return B_OK;
}


status_t
SPIBus::AcquireBus()
{
	if (fController->acquire_bus != NULL)
		return fController->acquire_bus(fCookie);
	return B_OK;
}


void
SPIBus::ReleaseBus()
{
	if (fController->release_bus != NULL)
		fController->release_bus(fCookie);
}


status_t
spi_validate_transfers(const spi_device_config* config,
	const spi_transfer* transfers, size_t count)
{
	if (config == NULL || transfers == NULL || count == 0
		|| count > SPI_MAX_TRANSFERS) {
		return B_BAD_VALUE;
	}
	if (config->speed_hz == 0 || config->bits_per_word == 0
		|| config->bits_per_word > 32) {
		return B_BAD_VALUE;
	}

	for (size_t i = 0; i < count; i++) {
		if (transfers[i].length > SPI_MAX_TRANSFER_LENGTH)
			return B_BAD_VALUE;
		// a delay is at most a second: devices ask for micro seconds
		if (transfers[i].delay_us > 1000000)
			return B_BAD_VALUE;
	}

	return B_OK;
}


//	#pragma mark - module interface


static status_t
spi_init_bus(device_node* node, void** _bus)
{
	CALLED();
	uint8 pathID;
	if (gDeviceManager->get_attr_uint8(node, SPI_BUS_PATH_ID_ITEM, &pathID,
			false) != B_OK) {
		return B_ERROR;
	}

	SPIBus* bus = new(std::nothrow) SPIBus(node, pathID);
	if (bus == NULL)
		return B_NO_MEMORY;

	status_t result = bus->InitCheck();
	if (result != B_OK) {
		ERROR("failed to set up spi bus object: %s\n", strerror(result));
		delete bus;
		return result;
	}

	*_bus = bus;
	return B_OK;
}


static void
spi_uninit_bus(void* _bus)
{
	CALLED();
	delete (SPIBus*)_bus;
}


static status_t
spi_scan_bus(void* _bus)
{
	return ((SPIBus*)_bus)->Scan();
}


static status_t
spi_bus_transfer(spi_bus _bus, const spi_device_config* config,
	const spi_transfer* transfers, size_t count)
{
	return ((SPIBus*)_bus)->Transfer(config, transfers, count);
}


static status_t
spi_bus_acquire_bus(spi_bus _bus)
{
	return ((SPIBus*)_bus)->AcquireBus();
}


static void
spi_bus_release_bus(spi_bus _bus)
{
	((SPIBus*)_bus)->ReleaseBus();
}


static status_t
std_ops(int32 op, ...)
{
	switch (op) {
		case B_MODULE_INIT:
		case B_MODULE_UNINIT:
			return B_OK;

		default:
			break;
	}

	return B_ERROR;
}


spi_bus_interface gSPIBusModule = {
	{
		{
			SPI_BUS_MODULE_NAME,
			0,
			std_ops
		},

		NULL, // supported devices
		NULL, // register node
		spi_init_bus,
		spi_uninit_bus,
		spi_scan_bus, // register child devices
		NULL, // rescan
	},

	spi_bus_transfer,
	spi_bus_acquire_bus,
	spi_bus_release_bus,
};
