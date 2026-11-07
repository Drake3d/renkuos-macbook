/*
 * Copyright 2026, RenkuOS Contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


//!	Intel LPSS SPI controller (PXA2xx style SSP), programmed I/O only.
//
//	Peripherals are found through ACPI: every device whose _CRS holds a
//	SpiSerialBus resource naming this controller becomes a child of the SPI
//	bus. Transfers run from the caller's thread with the FIFO polled; they
//	are short (the Apple keyboard sends 256 byte packets), and this keeps
//	the controller free of interrupt and DMA set up until a peripheral needs
//	more.


#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ACPI.h>
#include <KernelExport.h>

#include "pch_spi.h"


device_manager_info* gDeviceManager;
spi_for_controller_interface* gSPI;
acpi_module_info* gACPI;


static inline uint32
read32(pch_spi_info* bus, uint32 offset)
{
	return *(volatile uint32*)(bus->registers + offset);
}


static inline void
write32(pch_spi_info* bus, uint32 offset, uint32 value)
{
	*(volatile uint32*)(bus->registers + offset) = value;
}


static void
set_chip_select(pch_spi_info* bus, const spi_device_config* config,
	bool assert)
{
	// The line is active low unless the device says otherwise.
	bool high = assert == ((config->mode & SPI_CS_HIGH) != 0);
	uint32 value = read32(bus, PCH_LPSS_CS_CONTROL) | LPSS_CS_SW_MODE;
	if (high)
		value |= LPSS_CS_HIGH;
	else
		value &= ~LPSS_CS_HIGH;
	write32(bus, PCH_LPSS_CS_CONTROL, value);
}


static void
delay(uint32 us)
{
	if (us == 0)
		return;
	if (us < 1000)
		spin(us);
	else
		snooze(us);
}


static status_t
configure_port(pch_spi_info* bus, const spi_device_config* config)
{
	if ((config->mode & SPI_LSB_FIRST) != 0)
		return B_NOT_SUPPORTED;
	if (config->chip_select != 0) {
		// Sunrise Point ports have a single chip select.
		return B_NOT_SUPPORTED;
	}
	if (config->bits_per_word < 4 || config->bits_per_word > 32)
		return B_NOT_SUPPORTED;

	uint32 divider = (bus->clock + config->speed_hz - 1) / config->speed_hz;
	if (divider == 0)
		divider = 1;
	if (divider > 0x1000)
		divider = 0x1000;

	uint32 sscr0 = SSCR0_RIM | SSCR0_TIM
		| (((divider - 1) << SSCR0_SCR_SHIFT) & SSCR0_SCR_MASK);
	if (config->bits_per_word > 16) {
		sscr0 |= SSCR0_EDSS
			| ((config->bits_per_word - 17) & SSCR0_DSS_MASK);
	} else
		sscr0 |= (config->bits_per_word - 1) & SSCR0_DSS_MASK;

	uint32 sscr1 = 0;
	if ((config->mode & SPI_CPOL) != 0)
		sscr1 |= SSCR1_SPO;
	if ((config->mode & SPI_CPHA) != 0)
		sscr1 |= SSCR1_SPH;

	// The port must be disabled while it is reconfigured.
	write32(bus, PCH_SSP_SSCR0, sscr0 & ~SSCR0_SSE);
	write32(bus, PCH_SSP_SSCR1, sscr1);
	write32(bus, PCH_SSP_SSTO, 0);
	write32(bus, PCH_SSP_SSPSP, 0);
	write32(bus, PCH_SSP_SSSR, SSSR_ROR | SSSR_TINT | SSSR_TUR);
	write32(bus, PCH_SSP_SSCR0, sscr0 | SSCR0_SSE);

	// Drop whatever an earlier user left in the receive FIFO.
	for (int i = 0; i < 256
			&& (read32(bus, PCH_SSP_SSSR) & SSSR_RNE) != 0; i++) {
		read32(bus, PCH_SSP_SSDR);
	}

	TRACE("configured: divider %" B_PRIu32 " (%" B_PRIu32 " Hz), %u bits, "
		"mode %u\n", divider, bus->clock / divider, config->bits_per_word,
		config->mode);
	return B_OK;
}


static status_t
pio_transfer(pch_spi_info* bus, const spi_device_config* config,
	const spi_transfer& transfer)
{
	size_t wordSize = config->bits_per_word <= 8 ? 1
		: config->bits_per_word <= 16 ? 2 : 4;
	if (transfer.length % wordSize != 0)
		return B_BAD_VALUE;

	size_t words = transfer.length / wordSize;
	const uint8* tx = (const uint8*)transfer.tx_buffer;
	uint8* rx = (uint8*)transfer.rx_buffer;

	// Allow ten times the time on the wire, and at least 10 ms.
	bigtime_t wire = (bigtime_t)transfer.length * 8 * 1000000
		/ config->speed_hz;
	bigtime_t deadline = system_time() + max_c(wire * 10, 10000);

	size_t sent = 0;
	size_t received = 0;
	while (received < words) {
		uint32 status = read32(bus, PCH_SSP_SSSR);

		if (sent < words && sent - received < PCH_SSP_FIFO_WINDOW
			&& (status & SSSR_TNF) != 0) {
			uint32 value = 0;
			if (tx != NULL) {
				const uint8* word = tx + sent * wordSize;
				if (wordSize == 1)
					value = word[0];
				else if (wordSize == 2)
					value = *(const uint16*)word;
				else
					value = *(const uint32*)word;
			}
			write32(bus, PCH_SSP_SSDR, value);
			sent++;
		}

		if ((status & SSSR_RNE) != 0) {
			uint32 value = read32(bus, PCH_SSP_SSDR);
			if (rx != NULL) {
				uint8* word = rx + received * wordSize;
				if (wordSize == 1)
					word[0] = (uint8)value;
				else if (wordSize == 2)
					*(uint16*)word = (uint16)value;
				else
					*(uint32*)word = value;
			}
			received++;
			continue;
		}

		if ((status & SSSR_ROR) != 0) {
			write32(bus, PCH_SSP_SSSR, SSSR_ROR);
			ERROR("receive overrun\n");
			return B_IO_ERROR;
		}

		if (system_time() > deadline) {
			ERROR("transfer timed out, %" B_PRIuSIZE " of %" B_PRIuSIZE
				" words\n", received, words);
			return B_TIMED_OUT;
		}
	}

	return B_OK;
}


//	#pragma mark - SIM interface


static void
set_sim(spi_bus_cookie cookie, spi_bus bus)
{
	CALLED();
	((pch_spi_info*)cookie)->bus = bus;
}


static status_t
transfer(spi_bus_cookie cookie, const spi_device_config* config,
	const spi_transfer* transfers, size_t count)
{
	pch_spi_info* bus = (pch_spi_info*)cookie;

	status_t status = configure_port(bus, config);
	if (status != B_OK)
		return status;

	set_chip_select(bus, config, true);
	for (size_t i = 0; i < count && status == B_OK; i++) {
		if (transfers[i].length > 0)
			status = pio_transfer(bus, config, transfers[i]);
		if (status != B_OK)
			break;

		delay(transfers[i].delay_us);

		if (transfers[i].cs_change && i + 1 < count) {
			set_chip_select(bus, config, false);
			spin(10);
			set_chip_select(bus, config, true);
		}
	}
	set_chip_select(bus, config, false);

	// Leave the port off between messages.
	write32(bus, PCH_SSP_SSCR0, read32(bus, PCH_SSP_SSCR0) & ~SSCR0_SSE);
	return status;
}


static status_t
acquire_bus(spi_bus_cookie cookie)
{
	return mutex_lock(&((pch_spi_info*)cookie)->lock);
}


static void
release_bus(spi_bus_cookie cookie)
{
	mutex_unlock(&((pch_spi_info*)cookie)->lock);
}


//	#pragma mark - ACPI enumeration


struct spi_crs {
	bool				found;
	spi_device_config	config;
	acpi_handle			controller;
	char				source[64];	// the controller as the resource names it
};


static status_t
evaluate_integer(acpi_handle handle, const char* method, int64* value)
{
	acpi_object_type object;
	acpi_data buffer;
	buffer.pointer = &object;
	buffer.length = sizeof(object);

	status_t status = gACPI->evaluate_method(handle, method, NULL, &buffer);
	if (status != B_OK)
		return status;
	if (object.object_type != ACPI_TYPE_INTEGER)
		return B_BAD_VALUE;
	*value = object.integer.integer;
	return B_OK;
}


static acpi_status
parse_crs(ACPI_RESOURCE* resource, void* context)
{
	spi_crs* crs = (spi_crs*)context;
	if (crs->found || resource->Type != ACPI_RESOURCE_TYPE_SERIAL_BUS
		|| resource->Data.CommonSerialBus.Type
			!= ACPI_RESOURCE_SERIAL_TYPE_SPI) {
		return AE_OK;
	}

	const ACPI_RESOURCE_SPI_SERIALBUS& spi = resource->Data.SpiSerialBus;
	crs->found = true;
	crs->config.chip_select = spi.DeviceSelection;
	crs->config.speed_hz = spi.ConnectionSpeed;
	crs->config.bits_per_word = spi.DataBitLength;
	crs->config.mode = 0;
	if (spi.ClockPhase == ACPI_SPI_SECOND_PHASE)
		crs->config.mode |= SPI_CPHA;
	if (spi.ClockPolarity == ACPI_SPI_START_HIGH)
		crs->config.mode |= SPI_CPOL;
	if (spi.DevicePolarity == ACPI_SPI_ACTIVE_HIGH)
		crs->config.mode |= SPI_CS_HIGH;

	crs->controller = NULL;
	crs->source[0] = '\0';
	if (spi.ResourceSource.StringPtr != NULL) {
		strlcpy(crs->source, spi.ResourceSource.StringPtr, sizeof(crs->source));
		gACPI->get_handle(NULL, spi.ResourceSource.StringPtr, &crs->controller);
	}

	return AE_OK;
}


struct scan_context {
	pch_spi_info*	bus;
	bool			children;	// walking the controller's own children
	int32			found;
};


//!	Checks one ACPI device and registers it if it is on this bus. Says why
//	a device is passed over when it is one of the controller's children or
//	looks like an Apple device, so a missing peripheral can be traced from
//	the syslog alone.
static void
scan_device(scan_context* context, acpi_handle object, const char* path,
	const char* hid, char** cidList, bool haveInfo)
{
	pch_spi_info* bus = context->bus;
	bool verbose = context->children
		|| (hid != NULL && strncmp(hid, "APP", 3) == 0);
	const char* name = hid != NULL ? hid : "no _HID";

	int64 sta;
	if (evaluate_integer(object, "_STA", &sta) == B_OK
		&& (sta & ACPI_STA_DEVICE_PRESENT) == 0) {
		if (verbose) {
			TRACE_ALWAYS("%s (%s): _STA 0x%" B_PRIx64 ", not present\n", path,
				name, sta);
		}
		return;
	}

	spi_crs crs = {};
	status_t status = gACPI->walk_resources(object, (ACPI_STRING)"_CRS",
		parse_crs, &crs);
	if (status != B_OK || !crs.found) {
		// Told it is running macOS, Apple's firmware leaves SpiSerialBus out
		// of _CRS and gives the SPI settings as device properties from _DSM
		// instead (the keyboard below SPI1 on the 2016/2017 MacBook Pro).
		// Take such a device when it sits right below us, on chip select 0
		// with default settings; its driver reads the properties.
		acpi_handle dsm;
		if (context->children
			&& gACPI->get_handle(object, "_DSM", &dsm) == B_OK) {
			TRACE_ALWAYS("%s (%s): no SpiSerialBus in _CRS, taking it as a "
				"child with _DSM properties\n", path, name);
			crs.found = true;
			crs.controller = bus->acpiHandle;
		} else {
			if (verbose) {
				if (status != B_OK) {
					// walk_resources() passes the ACPI status through.
					TRACE_ALWAYS("%s (%s): _CRS failed, ACPI status 0x%"
						B_PRIx32 "\n", path, name, (uint32)status);
				} else {
					TRACE_ALWAYS("%s (%s): no SpiSerialBus in _CRS\n", path,
						name);
				}
			}
			return;
		}
	}

	// The resource names its controller. Away from our own children, only
	// take devices that name us; among them, also those that name nothing
	// we can resolve.
	if (crs.controller != bus->acpiHandle
		&& (!context->children || crs.controller != NULL)) {
		TRACE_ALWAYS("%s (%s): SpiSerialBus names \"%s\"%s, not this "
			"controller\n", path, name, crs.source,
			crs.controller == NULL ? " (unresolved)" : "");
		return;
	}

	if (!haveInfo) {
		TRACE_ALWAYS("%s: SPI device, but its _HID/_CID can't be read\n", path);
		return;
	}

	if (crs.config.speed_hz == 0)
		crs.config.speed_hz = 1000000;
	if (crs.config.bits_per_word == 0)
		crs.config.bits_per_word = 8;

	status = gSPI->register_device(bus->bus, &crs.config, hid, cidList,
		object);
	if (status == B_OK)
		context->found++;
	else
		ERROR("registering %s failed: %s\n", path, strerror(status));
}


static acpi_status
scan_callback(acpi_handle object, uint32 nestingLevel, void* _context,
	void** returnValue)
{
	scan_context* context = (scan_context*)_context;

	acpi_data path;
	path.pointer = NULL;
	path.length = ACPI_ALLOCATE_BUFFER;
	if (gACPI->ns_handle_to_pathname(object, &path) != B_OK)
		return AE_OK;

	char* hid = NULL;
	char* cidList[8] = { NULL };
	bool haveInfo = gACPI->get_device_info((const char*)path.pointer, &hid,
		cidList, 8, NULL, NULL) == B_OK;

	scan_device(context, object, (const char*)path.pointer, hid, cidList,
		haveInfo);

	free(hid);
	for (int i = 0; cidList[i] != NULL; i++)
		free(cidList[i]);
	free(path.pointer);

	// Never end the walk early: a sibling may still be ours.
	return AE_OK;
}


static status_t
scan_bus(spi_bus_cookie cookie)
{
	CALLED();
	pch_spi_info* bus = (pch_spi_info*)cookie;
	if (bus->acpiHandle == NULL) {
		TRACE_ALWAYS("no ACPI node for this controller, nothing to scan\n");
		return B_OK;
	}

	acpi_data path;
	path.pointer = NULL;
	path.length = ACPI_ALLOCATE_BUFFER;
	if (gACPI->ns_handle_to_pathname(bus->acpiHandle, &path) == B_OK) {
		TRACE_ALWAYS("scanning ACPI below %s\n", (char*)path.pointer);
		free(path.pointer);
	}

	// Peripherals usually sit right below their controller, as the Apple
	// keyboard does below SPI1. Only if none is there, look through the
	// whole system bus for devices naming this controller.
	scan_context context = { bus, true, 0 };
	gACPI->walk_namespace(bus->acpiHandle, ACPI_TYPE_DEVICE, 1,
		scan_callback, NULL, &context, NULL);

	if (context.found == 0) {
		acpi_handle systemBus;
		if (gACPI->get_handle(NULL, "\\_SB", &systemBus) == B_OK) {
			TRACE_ALWAYS("nothing below the controller, scanning all of "
				"\\_SB\n");
			context.children = false;
			gACPI->walk_namespace(systemBus, ACPI_TYPE_DEVICE, 16,
				scan_callback, NULL, &context, NULL);
		}
	}

	TRACE_ALWAYS("found %" B_PRId32 " device(s)\n", context.found);
	return B_OK;
}


//	#pragma mark - driver module


static status_t
init_bus(device_node* node, void** busCookie)
{
	CALLED();

	driver_module_info* driver;
	pch_spi_info* bus;
	device_node* parent = gDeviceManager->get_parent_node(node);
	gDeviceManager->get_driver(parent, &driver, (void**)&bus);
	gDeviceManager->put_node(parent);

	bus->registers_area = map_physical_memory("PCH SPI registers",
		bus->base_addr, bus->map_size, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&bus->registers);
	if (bus->registers_area < 0) {
		ERROR("mapping registers failed\n");
		return bus->registers_area;
	}

	// Take the function and its DMA out of reset.
	write32(bus, PCH_LPSS_RESETS, 0);
	write32(bus, PCH_LPSS_RESETS, LPSS_RESETS_FUNC | LPSS_RESETS_IDMA);

	// Use the clock divider the firmware set, Apple's EFI drives the
	// keyboard already; only if the clock is off, run it undivided.
	uint32 clock = read32(bus, PCH_LPSS_CLOCK_PARAMS);
	uint32 m = (clock >> LPSS_CLOCK_M_SHIFT) & LPSS_CLOCK_M_MASK;
	uint32 n = (clock >> LPSS_CLOCK_N_SHIFT) & LPSS_CLOCK_N_MASK;
	if ((clock & LPSS_CLOCK_EN) == 0 || m == 0 || n == 0 || m > n) {
		m = n = 1;
		clock = LPSS_CLOCK_EN | (m << LPSS_CLOCK_M_SHIFT)
			| (n << LPSS_CLOCK_N_SHIFT);
		write32(bus, PCH_LPSS_CLOCK_PARAMS, clock | LPSS_CLOCK_UPDATE);
	}
	bus->clock = (uint32)((uint64)PCH_LPSS_INPUT_CLOCK * m / n);

	// Software chip select, released.
	write32(bus, PCH_LPSS_CS_CONTROL, LPSS_CS_SW_MODE | LPSS_CS_HIGH);
	write32(bus, PCH_SSP_SSCR0, 0);

	mutex_init(&bus->lock, "pch_spi");

	TRACE_ALWAYS("controller at 0x%" B_PRIxPHYSADDR ", clock %" B_PRIu32
		" Hz\n", bus->base_addr, bus->clock);

	*busCookie = bus;
	return B_OK;
}


static void
uninit_bus(void* busCookie)
{
	pch_spi_info* bus = (pch_spi_info*)busCookie;

	write32(bus, PCH_SSP_SSCR0, 0);
	mutex_destroy(&bus->lock);
	if (bus->registers_area >= 0)
		delete_area(bus->registers_area);
	bus->registers_area = -1;
}


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&gDeviceManager },
	{ B_ACPI_MODULE_NAME, (module_info**)&gACPI },
	{ SPI_FOR_CONTROLLER_MODULE_NAME, (module_info**)&gSPI },
	{}
};


static spi_sim_interface sPchSpiDeviceModule = {
	{
		{
			PCH_SPI_SIM_MODULE_NAME,
			0,
			NULL
		},

		NULL,	// supports device
		NULL,	// register device
		init_bus,
		uninit_bus,
		NULL,	// register child devices
		NULL,	// rescan
		NULL, 	// device removed
	},

	set_sim,
	transfer,
	scan_bus,
	acquire_bus,
	release_bus,
};


module_info* modules[] = {
	(module_info*)&gPchSpiPciDevice,
	(module_info*)&sPchSpiDeviceModule,
	NULL
};
