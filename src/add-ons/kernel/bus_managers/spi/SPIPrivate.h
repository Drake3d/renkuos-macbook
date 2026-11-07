/*
 * Copyright 2026, RenkuOS Contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */
#ifndef SPI_PRIVATE_H
#define SPI_PRIVATE_H


#include <new>
#include <stdio.h>
#include <string.h>

#include <lock.h>
#include <spi.h>


//#define SPI_TRACE
#ifdef SPI_TRACE
#	define TRACE(x...)		dprintf("\33[33mspi:\33[0m " x)
#else
#	define TRACE(x...)
#endif
#define TRACE_ALWAYS(x...)	dprintf("\33[33mspi:\33[0m " x)
#define ERROR(x...)			TRACE_ALWAYS(x)
#define CALLED() 			TRACE("CALLED %s\n", __PRETTY_FUNCTION__)


#define SPI_PATHID_GENERATOR "spi/path_id"

// Transfers longer than this are refused, so that a confused peripheral
// driver cannot hold the bus for long.
#define SPI_MAX_TRANSFER_LENGTH		(64 * 1024)
#define SPI_MAX_TRANSFERS			16


class SPIBus;

extern device_manager_info* gDeviceManager;
extern spi_bus_interface gSPIBusModule;
extern spi_device_interface gSPIDeviceModule;


class SPIDevice {
public:
								SPIDevice(device_node* node, SPIBus* bus,
									const spi_device_config& config);

			status_t			GetConfig(spi_device_config* config);
			status_t			SetConfig(const spi_device_config* config);
			status_t			Transfer(const spi_transfer* transfers,
									size_t count);
			status_t			AcquireBus();
			void				ReleaseBus();

private:
			device_node*		fNode;
			SPIBus*				fBus;
			spi_device_config	fConfig;
};


class SPIBus {
public:
								SPIBus(device_node* node, uint8 id);

			status_t			InitCheck() const { return fStatus; }

			status_t			Transfer(const spi_device_config* config,
									const spi_transfer* transfers,
									size_t count);
			status_t			Scan();
			status_t			RegisterDevice(const spi_device_config* config,
									const char* hid, char** cid,
									acpi_handle acpiHandle);
			status_t			AcquireBus();
			void				ReleaseBus();

private:
			status_t			fStatus;
			device_node*		fNode;
			uint8				fID;
			spi_sim_interface*	fController;
			void*				fCookie;
};


status_t spi_validate_transfers(const spi_device_config* config,
	const spi_transfer* transfers, size_t count);


#endif // SPI_PRIVATE_H
