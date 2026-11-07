/*
 * Copyright 2026, RenkuOS Contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


#include "SPIPrivate.h"


SPIDevice::SPIDevice(device_node* node, SPIBus* bus,
	const spi_device_config& config)
	:
	fNode(node),
	fBus(bus),
	fConfig(config)
{
	CALLED();
}


status_t
SPIDevice::GetConfig(spi_device_config* config)
{
	if (config == NULL)
		return B_BAD_VALUE;
	*config = fConfig;
	return B_OK;
}


status_t
SPIDevice::SetConfig(const spi_device_config* config)
{
	if (config == NULL || config->speed_hz == 0
		|| config->bits_per_word == 0 || config->bits_per_word > 32) {
		return B_BAD_VALUE;
	}

	// The chip select is wired; a driver cannot move to another one.
	if (config->chip_select != fConfig.chip_select)
		return B_NOT_ALLOWED;

	fConfig = *config;
	return B_OK;
}


status_t
SPIDevice::Transfer(const spi_transfer* transfers, size_t count)
{
	return fBus->Transfer(&fConfig, transfers, count);
}


status_t
SPIDevice::AcquireBus()
{
	return fBus->AcquireBus();
}


void
SPIDevice::ReleaseBus()
{
	fBus->ReleaseBus();
}


//	#pragma mark - module interface


static status_t
spi_init_device(device_node* node, void** _device)
{
	CALLED();
	SPIBus* bus;

	{
		device_node* parent = gDeviceManager->get_parent_node(node);
		gDeviceManager->get_driver(parent, NULL, (void**)&bus);
		gDeviceManager->put_node(parent);
	}

	spi_device_config config;
	if (gDeviceManager->get_attr_uint16(node, SPI_DEVICE_CHIP_SELECT_ITEM,
			&config.chip_select, false) != B_OK
		|| gDeviceManager->get_attr_uint32(node, SPI_DEVICE_SPEED_ITEM,
			&config.speed_hz, false) != B_OK
		|| gDeviceManager->get_attr_uint8(node, SPI_DEVICE_MODE_ITEM,
			&config.mode, false) != B_OK
		|| gDeviceManager->get_attr_uint8(node, SPI_DEVICE_BITS_ITEM,
			&config.bits_per_word, false) != B_OK) {
		return B_ERROR;
	}

	SPIDevice* device = new(std::nothrow) SPIDevice(node, bus, config);
	if (device == NULL)
		return B_NO_MEMORY;

	*_device = device;
	return B_OK;
}


static void
spi_uninit_device(void* _device)
{
	CALLED();
	delete (SPIDevice*)_device;
}


static status_t
spi_get_config(spi_device _device, spi_device_config* config)
{
	return ((SPIDevice*)_device)->GetConfig(config);
}


static status_t
spi_set_config(spi_device _device, const spi_device_config* config)
{
	return ((SPIDevice*)_device)->SetConfig(config);
}


static status_t
spi_transfer_device(spi_device _device, const spi_transfer* transfers,
	size_t count)
{
	return ((SPIDevice*)_device)->Transfer(transfers, count);
}


static status_t
spi_acquire_bus(spi_device _device)
{
	return ((SPIDevice*)_device)->AcquireBus();
}


static void
spi_release_bus(spi_device _device)
{
	((SPIDevice*)_device)->ReleaseBus();
}


static status_t
std_ops(int32 op, ...)
{
	switch (op) {
		case B_MODULE_INIT:
		{
			// The device module calls into the bus object directly, but
			// keeps the bus module loaded while devices exist.
			module_info* dummy;
			return get_module(SPI_BUS_MODULE_NAME, &dummy);
		}
		case B_MODULE_UNINIT:
			return put_module(SPI_BUS_MODULE_NAME);

		default:
			return B_ERROR;
	}
}


spi_device_interface gSPIDeviceModule = {
	{
		{
			SPI_DEVICE_MODULE_NAME,
			0,
			std_ops
		},

		NULL,	// supported devices
		NULL,	// register node
		spi_init_device,
		spi_uninit_device,
		NULL,	// register child devices
		NULL,	// rescan
		NULL	// device removed
	},
	spi_get_config,
	spi_set_config,
	spi_transfer_device,
	spi_acquire_bus,
	spi_release_bus,
};
