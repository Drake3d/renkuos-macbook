/*
 * Copyright 2026, RenkuOS Contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */
#ifndef _SPI_H_
#define _SPI_H_


#include <ACPI.h>
#include <device_manager.h>
#include <KernelExport.h>


// SPI mode bits, as in the SPI clock polarity and phase convention.
#define SPI_CPHA			0x01	// sample on the second clock edge
#define SPI_CPOL			0x02	// clock idles high
#define SPI_MODE_0			0
#define SPI_MODE_1			SPI_CPHA
#define SPI_MODE_2			SPI_CPOL
#define SPI_MODE_3			(SPI_CPOL | SPI_CPHA)
#define SPI_CS_HIGH			0x04	// chip select is active high
#define SPI_LSB_FIRST		0x08	// send the least significant bit first


typedef struct spi_device_config {
	uint32		speed_hz;			// maximum clock rate
	uint16		chip_select;
	uint8		mode;				// SPI_CPHA | SPI_CPOL | ...
	uint8		bits_per_word;
} spi_device_config;


// One part of a message. Chip select is held asserted for the whole message.
// A transfer with a length of 0 only waits, which is how a device asks for a
// delay between asserting chip select and the first clock.
typedef struct spi_transfer {
	const void*	tx_buffer;			// NULL sends zeroes
	void*		rx_buffer;			// NULL drops what is received
	size_t		length;
	uint32		delay_us;			// wait after this transfer
	bool		cs_change;			// release chip select after this transfer
									// and assert it again before the next
} spi_transfer;


// bus/device handle
typedef void* spi_bus;
typedef void* spi_device;


// Device node

#define SPI_DEVICE_CHIP_SELECT_ITEM	"spi/chip_select"		// uint16
#define SPI_DEVICE_SPEED_ITEM		"spi/speed"				// uint32, Hz
#define SPI_DEVICE_MODE_ITEM		"spi/mode"				// uint8
#define SPI_DEVICE_BITS_ITEM		"spi/bits_per_word"		// uint8

#define SPI_DEVICE_TYPE_NAME "spi/device/v1"

// bus manager device interface for peripheral drivers
typedef struct {
	driver_module_info info;

	// The configuration starts out as described by the firmware. A driver
	// that knows better, from the device itself or from its _DSM, may
	// change it.
	status_t (*get_config)(spi_device cookie, spi_device_config* config);
	status_t (*set_config)(spi_device cookie, const spi_device_config* config);

	status_t (*transfer)(spi_device cookie, const spi_transfer* transfers,
		size_t count);

	status_t (*acquire_bus)(spi_device cookie);
	void (*release_bus)(spi_device cookie);
} spi_device_interface;

#define SPI_DEVICE_MODULE_NAME "bus_managers/spi/device/driver_v1"


// Bus node

#define SPI_BUS_PATH_ID_ITEM "spi/path_id"

#define SPI_BUS_TYPE_NAME "spi/bus"

typedef struct {
	driver_module_info info;

	status_t (*transfer)(spi_bus cookie, const spi_device_config* config,
		const spi_transfer* transfers, size_t count);
	status_t (*acquire_bus)(spi_bus cookie);
	void (*release_bus)(spi_bus cookie);
} spi_bus_interface;

#define SPI_BUS_MODULE_NAME "bus_managers/spi/bus/driver_v1"


// Interface for controllers

typedef struct {
	driver_module_info info;

	// Registers a peripheral found in the firmware tables. The bus manager
	// copies everything it is given.
	status_t (*register_device)(spi_bus bus, const spi_device_config* config,
		const char* hid, char** cid, acpi_handle acpiHandle);
} spi_for_controller_interface;

#define SPI_FOR_CONTROLLER_MODULE_NAME "bus_managers/spi/controller/driver_v1"


// Controller node

#define SPI_CONTROLLER_TYPE_NAME "bus/spi/v1"

typedef void* spi_bus_cookie;


// Bus manager interface used by SPI controller drivers.
typedef struct {
	driver_module_info info;

	void (*set_spi_bus)(spi_bus_cookie cookie, spi_bus bus);

	// Runs one message: asserts the chip select of the configuration,
	// performs the transfers in order and releases it again. The caller
	// holds the bus.
	status_t (*transfer)(spi_bus_cookie cookie, const spi_device_config* config,
		const spi_transfer* transfers, size_t count);

	status_t (*scan_bus)(spi_bus_cookie cookie);

	status_t (*acquire_bus)(spi_bus_cookie cookie);
	void (*release_bus)(spi_bus_cookie cookie);
} spi_sim_interface;


#endif	/* _SPI_H_ */
