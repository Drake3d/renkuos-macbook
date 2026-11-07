/*
 * Copyright 2026, RenkuOS Contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


//!	SPI bus manager. Mirrors the layout of the I2C bus manager: a controller
//	node gets an "SPI bus" child, and every peripheral the controller finds
//	in the firmware tables becomes an "SPI device" child of that bus, for
//	peripheral drivers to bind to by bus "spi" and ACPI hardware ID.


#include "SPIPrivate.h"


device_manager_info* gDeviceManager = NULL;


//	#pragma mark -


static status_t
spi_added_device(device_node* parent)
{
	CALLED();

	int32 pathID = gDeviceManager->create_id(SPI_PATHID_GENERATOR);
	if (pathID < 0) {
		ERROR("cannot register spi controller - out of path IDs\n");
		return B_ERROR;
	}

	device_attr attributes[] = {
		// info about device
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE, { .string = "SPI bus" }},
		{ B_DEVICE_BUS, B_STRING_TYPE, { .string = "spi" }},
		{ SPI_BUS_PATH_ID_ITEM, B_UINT8_TYPE, { .ui8 = (uint8)pathID }},
		{ NULL }
	};

	return gDeviceManager->register_node(parent, SPI_BUS_MODULE_NAME,
		attributes, NULL, NULL);
}


static status_t
spi_register_device(spi_bus _bus, const spi_device_config* config,
	const char* hid, char** cid, acpi_handle acpiHandle)
{
	CALLED();
	if (config == NULL)
		return B_BAD_VALUE;

	SPIBus* bus = (SPIBus*)_bus;
	return bus->RegisterDevice(config, hid, cid, acpiHandle);
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


//	#pragma mark -


spi_for_controller_interface gSPIForControllerModule = {
	{
		{
			SPI_FOR_CONTROLLER_MODULE_NAME,
			0,
			&std_ops
		},

		NULL, // supported devices
		spi_added_device,
		NULL,
		NULL,
		NULL
	},

	spi_register_device,
};


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&gDeviceManager },
	{}
};


module_info* modules[] = {
	(module_info*)&gSPIForControllerModule,
	(module_info*)&gSPIBusModule,
	(module_info*)&gSPIDeviceModule,
	NULL
};
