/*
 * Copyright 2026, RenkuOS Contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */
#ifndef _APPLE_SPI_DRIVER_H_
#define _APPLE_SPI_DRIVER_H_


#include <Drivers.h>
#include <KernelExport.h>
#include <OS.h>
#include <util/kernel_cpp.h>

#include "DeviceList.h"


// Used by the shared HID code to name what it publishes:
// input/keyboard/spi/N, "Apple SPI Keyboard".
#define DRIVER_NAME			"apple_spi"
#define DEVICE_PATH_SUFFIX	"spi"
#define DEVICE_NAME			"Apple SPI"

extern DeviceList* gDeviceList;


//#define TRACE_APPLE_SPI
#ifdef TRACE_APPLE_SPI
#	define TRACE(x...) dprintf(DRIVER_NAME ": " x)
#else
#	define TRACE(x...)
#endif
#define ERROR(x...)			dprintf(DRIVER_NAME ": " x)
#define TRACE_ALWAYS(x...)	dprintf(DRIVER_NAME ": " x)
#define CALLED() 			TRACE("CALLED %s\n", __PRETTY_FUNCTION__)


#endif // _APPLE_SPI_DRIVER_H_
