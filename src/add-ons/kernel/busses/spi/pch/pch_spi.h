/*
 * Copyright 2026, RenkuOS Contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */
#ifndef _PCH_SPI_H
#define _PCH_SPI_H


extern "C" {
#	include "acpi.h"
}

#include <bus/PCI.h>
#include <lock.h>
#include <spi.h>


//#define TRACE_PCH_SPI
#ifdef TRACE_PCH_SPI
#	define TRACE(x...) dprintf("\33[33mpch_spi:\33[0m " x)
#else
#	define TRACE(x...) ;
#endif
#define TRACE_ALWAYS(x...)	dprintf("\33[33mpch_spi:\33[0m " x)
#define ERROR(x...)			dprintf("\33[33mpch_spi:\33[0m " x)
#define CALLED(x...)		TRACE("CALLED %s\n", __PRETTY_FUNCTION__)


// Installed below drivers/, not busses/: the device manager already looks
// there for Intel LPSS ports (PCI class 0x11/0x80), so the driver works from
// its own package on an unchanged kernel.
#define PCH_SPI_PCI_DEVICE_MODULE_NAME "drivers/spi/pch_spi/pci/driver_v1"
#define PCH_SPI_SIM_MODULE_NAME "drivers/spi/pch_spi/device/v1"


// Intel LPSS SPI: a PXA2xx style SSP port, followed by the LPSS private
// registers. Register names follow the PXA2xx documentation.

#define PCH_SSP_SSCR0			0x00
#define		SSCR0_DSS_MASK		0x0000000f	// data size - 1, 4 to 16 bits
#define		SSCR0_FRF_MASK		0x00000030	// 0: Motorola SPI
#define		SSCR0_SSE			(1u << 7)	// port enable
#define		SSCR0_SCR_SHIFT		8
#define		SSCR0_SCR_MASK		0x000fff00	// serial clock rate divider
#define		SSCR0_EDSS			(1u << 20)	// data size + 16
#define		SSCR0_RIM			(1u << 22)	// mask receive overrun
#define		SSCR0_TIM			(1u << 23)	// mask transmit underrun
#define PCH_SSP_SSCR1			0x04
#define		SSCR1_SPO			(1u << 3)	// clock polarity
#define		SSCR1_SPH			(1u << 4)	// clock phase
#define PCH_SSP_SSSR			0x08
#define		SSSR_TNF			(1u << 2)	// transmit FIFO not full
#define		SSSR_RNE			(1u << 3)	// receive FIFO not empty
#define		SSSR_BSY			(1u << 4)
#define		SSSR_ROR			(1u << 7)	// receive overrun, write 1 to clear
#define		SSSR_TINT			(1u << 19)	// receiver timeout, write 1 to clear
#define		SSSR_TUR			(1u << 21)	// transmit underrun, write 1 to clear
#define PCH_SSP_SSDR			0x10
#define PCH_SSP_SSTO			0x28
#define PCH_SSP_SSPSP			0x2c

// LPSS private registers (Sunrise Point layout)
#define PCH_LPSS_PRIV			0x200
#define PCH_LPSS_CLOCK_PARAMS	(PCH_LPSS_PRIV + 0x00)
#define		LPSS_CLOCK_EN		(1u << 0)
#define		LPSS_CLOCK_M_SHIFT	1
#define		LPSS_CLOCK_M_MASK	0x7fff
#define		LPSS_CLOCK_N_SHIFT	16
#define		LPSS_CLOCK_N_MASK	0x7fff
#define		LPSS_CLOCK_UPDATE	(1u << 31)
#define PCH_LPSS_RESETS			(PCH_LPSS_PRIV + 0x04)
#define		LPSS_RESETS_FUNC	(1u << 2)
#define		LPSS_RESETS_IDMA	0x3
#define PCH_LPSS_CS_CONTROL		(PCH_LPSS_PRIV + 0x24)
#define		LPSS_CS_SW_MODE		(1u << 0)	// chip select driven by software
#define		LPSS_CS_HIGH		(1u << 1)	// level of the chip select line

// The LPSS function clock on Sunrise Point and Union Point. Should the
// clock be slower on some part, the bus only runs slower than asked.
#define PCH_LPSS_INPUT_CLOCK	120000000

// Smallest FIFO among the supported parts; keeping no more than this many
// words in flight can never overrun the receive FIFO.
#define PCH_SSP_FIFO_WINDOW		16


extern device_manager_info* gDeviceManager;
extern spi_for_controller_interface* gSPI;
extern acpi_module_info* gACPI;
extern driver_module_info gPchSpiPciDevice;


typedef struct {
	device_node*			driver_node;
	spi_bus					bus;

	pci_device_module_info*	pci;
	pci_device*				device;
	pci_info				pciinfo;

	phys_addr_t				base_addr;
	uint64					map_size;
	area_id					registers_area;
	addr_t					registers;

	acpi_handle				acpiHandle;		// the controller's ACPI node
	uint32					clock;			// SSP clock in Hz

	mutex					lock;
} pch_spi_info;


#endif // _PCH_SPI_H
