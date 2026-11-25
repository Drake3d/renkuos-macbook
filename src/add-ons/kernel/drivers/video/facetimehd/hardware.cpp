/*
 * Copyright 2026, RenkuOS Contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


/*!	Bring-up of the FaceTime HD camera (Broadcom 14e4:1570): the link from
	PCIe to the camera's "S2" chip, its PLL and its DDR memory.

	Sources, as noted at each step:
	[F]	The patjak/facetimehd Linux driver (fthd_hw.c, fthd_ddr.c,
		fthd_reg.h), reverse engineered from Apple's driver. It is GPL-2.0;
		only facts were taken from it (register offsets, values and their
		order), no code. Register names follow it where it has them.
*/


#include "facetimehd.h"

#include <string.h>


// #pragma mark - registers [F]


// PCIe-to-S2 link
#define S2_PCIE_D000			0xd000
#define S2_PCIE_D104			0xd104
#define S2_PCIE_D108			0xd108
#define S2_PCIE_D120			0xd120
#define S2_PCIE_D124			0xd124
#define S2_PCIE_D128			0xd128
#define S2_PCIE_D12C			0xd12c

// PLL
#define S2_PLL_REFCLK			0x0004
#define S2_PLL_REFCLK_25MHZ		(1 << 3)
#define S2_PLL_CMU_STATUS		0x000c
#define S2_PLL_CMU_LOCKED		(1 << 15)
#define S2_PLL_CTRL_14			0x0014
#define S2_PLL_CTRL_20			0x0020
#define S2_PLL_CTRL_24			0x0024
#define S2_PLL_CTRL_2C			0x002c
#define S2_PLL_CTRL_9C			0x009c
#define S2_PLL_STATUS_A8		0x00a8
#define S2_PLL_BYPASS			(1 << 0)
#define S2_PLL_CTRL_100			0x0100
#define S2_PLL_CTRL_510			0x0510

// DDR controller
#define S2_DDR_2004				0x2004
#define S2_DDR_2008				0x2008
#define S2_DDR_2014				0x2014
#define S2_DDR_STATUS_2018		0x2018
#define S2_DDR_STATUS_BUSY		(1 << 0)
#define S2_DDR_20A0				0x20a0
#define S2_DDR_20A4				0x20a4
#define S2_DDR_20A8				0x20a8
#define S2_DDR_20B0				0x20b0
#define S2_20F8					0x20f8
#define S2_DDR_2118				0x2118
#define S2_DDR_PLL_STATUS_241C	0x241c
#define S2_DDR_PLL_241C_LOCKED	(1 << 10)
#define S2_DDR_PLL_STATUS_2444	0x2444
#define S2_DDR_PLL_2444_LOCKED	(1 << 13)
#define S2_3200					0x3200
#define S2_3204					0x3204
#define S2_3208					0x3208

// DDR40 PHY
#define S2_DDR40_PHY_PLL_STATUS	0x2810
#define S2_DDR40_PHY_PLL_LOCKED	(1 << 0)
#define S2_DDR40_PHY_PLL_CFG	0x2814
#define S2_DDR40_PHY_PLL_DIV	0x281c
#define S2_DDR40_PHY_AUX_CTL	0x2820
#define S2_DDR40_PHY_VDL_OVR_COARSE	0x2830
#define S2_DDR40_PHY_VDL_OVR_FINE	0x2834
#define S2_DDR40_PHY_ZQ_PVT_COMP_CTL	0x283c
#define S2_DDR40_PHY_DRV_PAD_CTL	0x2840
#define S2_DDR40_PHY_VDL_CTL	0x2848
#define S2_DDR40_PHY_VDL_STATUS	0x284c
#define S2_DDR40_PHY_VDL_CHAN_STATUS	0x2854
#define S2_DDR40_PHY_VTT_CTL	0x285c
#define S2_DDR40_PHY_VTT_CONNECTIONS	0x2864
#define S2_DDR40_PHY_VTT_OVERRIDE	0x2868
#define S2_DDR40_STRAP_CTL		0x28b0
#define S2_DDR40_STRAP_CTL_2	0x28b4
#define S2_DDR40_STRAP_STATUS	0x28b8
#define S2_DDR40_WL_RD_DATA_DLY	0x2b60
#define S2_DDR40_WL_READ_CTL	0x2b64
#define S2_DDR40_WL_IDLE_PAD_CTL	0x2ba0
#define S2_DDR40_WL_DRV_PAD_CTL	0x2ba4
#define S2_DDR40_WL_CLK_PAD_DISABLE	0x2ba8
#define S2_DDR40_WL_WR_PREAMBLE_MODE	0x2bac

// the page table the camera uses to reach host memory
#define S2_PAGE_TABLE			0x9000
#define S2_PAGE_TABLE_ENTRIES	0x1000


// #pragma mark - access


/*!	Polls an S2 register until the bits in \a mask are set (or clear, if
	\a set is false), \a tries times, \a interval microseconds apart.
*/
static status_t
s2_wait(fthd_device* device, uint32 offset, uint32 mask, bool set,
	int32 tries, bigtime_t interval)
{
	for (int32 i = 0; i < tries; i++) {
		uint32 value = s2_read(device, offset);
		if (((value & mask) != 0) == set)
			return B_OK;
		spin(interval);
	}
	return B_TIMED_OUT;
}


// #pragma mark - BARs


static status_t
map_bar(fthd_device* device, int bar, const char* name, area_id* _area,
	volatile uint8** _address, size_t* _size)
{
	const pci_info& info = device->pci;
	phys_addr_t address = info.u.h0.base_registers[bar];
	uint64 size = info.u.h0.base_register_sizes[bar];
	if ((info.u.h0.base_register_flags[bar] & PCI_address_type)
			== PCI_address_type_64) {
		address |= (uint64)info.u.h0.base_registers[bar + 1] << 32;
		size |= (uint64)info.u.h0.base_register_sizes[bar + 1] << 32;
	}
	if (address == 0 || size == 0) {
		FTHD_LOG("BAR %d is not set up\n", bar);
		return B_BAD_VALUE;
	}

	void* virtualAddress;
	area_id area = map_physical_memory(name, address, size,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		&virtualAddress);
	if (area < 0) {
		FTHD_LOG("mapping BAR %d failed: %s\n", bar, strerror(area));
		return area;
	}

	FTHD_LOG("BAR %d at 0x%" B_PRIxPHYSADDR ", %" B_PRIu64 " KB\n", bar,
		address, size / 1024);
	*_area = area;
	*_address = (volatile uint8*)virtualAddress;
	*_size = size;
	return B_OK;
}


status_t
fthd_map(fthd_device* device)
{
	device->s2_io_area = device->s2_mem_area = device->isp_io_area = -1;

	const pci_info& info = device->pci;
	gPCI->set_powerstate(info.bus, info.device, info.function,
		PCI_pm_state_d0);

	uint16 command = gPCI->read_pci_config(info.bus, info.device,
		info.function, PCI_command, 2);
	command |= PCI_command_memory | PCI_command_master;
	gPCI->write_pci_config(info.bus, info.device, info.function,
		PCI_command, 2, command);

	// The device hangs while streaming with active state power management
	// on [F]: turn L0s, L1 and clock power management off on our side of
	// the link.
	uint8 capability;
	if (gPCI->find_pci_capability(info.bus, info.device, info.function,
			PCI_cap_id_pcie, &capability) == B_OK) {
		device->pcie_capability = capability;
		uint16 linkControl = gPCI->read_pci_config(info.bus, info.device,
			info.function, capability + 0x10, 2);
		uint16 newControl = linkControl & ~(0x0003 | 0x0100);
		gPCI->write_pci_config(info.bus, info.device, info.function,
			capability + 0x10, 2, newControl);
		FTHD_LOG("PCIe link control 0x%04x -> 0x%04x\n", linkControl,
			newControl);
	} else
		FTHD_LOG("no PCIe capability\n");

	status_t status = map_bar(device, FTHD_BAR_S2_IO, "facetimehd S2 io",
		&device->s2_io_area, &device->s2_io, &device->s2_io_size);
	if (status == B_OK) {
		status = map_bar(device, FTHD_BAR_S2_MEM, "facetimehd S2 memory",
			&device->s2_mem_area, &device->s2_mem, &device->s2_mem_size);
	}
	if (status == B_OK) {
		status = map_bar(device, FTHD_BAR_ISP_IO, "facetimehd ISP io",
			&device->isp_io_area, &device->isp_io, &device->isp_io_size);
	}
	if (status != B_OK)
		fthd_unmap(device);
	return status;
}


void
fthd_unmap(fthd_device* device)
{
	if (device->s2_io_area >= 0)
		delete_area(device->s2_io_area);
	if (device->s2_mem_area >= 0)
		delete_area(device->s2_mem_area);
	if (device->isp_io_area >= 0)
		delete_area(device->isp_io_area);
	device->s2_io_area = device->s2_mem_area = device->isp_io_area = -1;
	device->s2_io = device->s2_mem = device->isp_io = NULL;
}


// #pragma mark - link and PLL [F]


static status_t
init_pcie_link(fthd_device* device)
{
	s2_write(device, S2_PCIE_D000, s2_read(device, S2_PCIE_D000) | 0x10);
	s2_write(device, S2_PCIE_D120, 0x1804);
	s2_write(device, S2_PCIE_D124, 0xac5800);
	s2_write(device, S2_PCIE_D120, 0x1804);
	uint32 check = s2_read(device, S2_PCIE_D124);
	if (check != 0xac5800) {
		FTHD_LOG("S2 link set-up failed: read back 0x%08" B_PRIx32 "\n",
			check);
		return B_IO_ERROR;
	}

	static const uint32 kLinkWrites[][2] = {
		{ 0x1f08, 0x80008610 },
		{ 0x1608, 0x8000fc00 },
		{ 0x1f08, 0x80008610 },
		{ 0x1708, 0x800005bf },
	};
	for (size_t i = 0; i < B_COUNT_OF(kLinkWrites); i++) {
		s2_write(device, S2_PCIE_D128, kLinkWrites[i][0]);
		s2_write(device, S2_PCIE_D12C, kLinkWrites[i][1]);
	}
	return B_OK;
}


static status_t
init_pll(fthd_device* device)
{
	bool ref25 = (s2_read(device, S2_PLL_REFCLK) & S2_PLL_REFCLK_25MHZ) != 0;

	// 450 MHz, the speed the Linux driver always uses
	if (ref25) {
		s2_write(device, S2_PLL_CTRL_510, 0x0048007d);
		s2_write(device, S2_PLL_CTRL_24, 0x19280904);
	} else {
		s2_write(device, S2_PLL_CTRL_20, 0x04b00000);
		s2_write(device, S2_PLL_CTRL_24, 0x14280904);
	}

	// reset
	s2_write(device, S2_PLL_CTRL_2C, 0x40);
	s2_write(device, S2_PLL_CTRL_2C, 0);
	s2_write(device, S2_PLL_CTRL_100, 0xbcbc1500);
	s2_write(device, S2_PLL_CTRL_14, 0);
	snooze(10000);
	s2_write(device, S2_PLL_CTRL_14, 3);

	bigtime_t start = system_time();
	if (s2_wait(device, S2_PLL_CMU_STATUS, S2_PLL_CMU_LOCKED, true, 10001, 10)
			!= B_OK) {
		FTHD_LOG("PLL did not lock (%s reference)\n", ref25 ? "25" : "24");
		return B_TIMED_OUT;
	}
	FTHD_LOG("PLL locked in %" B_PRIdBIGTIME " us, %s MHz reference\n",
		system_time() - start, ref25 ? "25" : "24");

	s2_write(device, S2_PLL_STATUS_A8,
		s2_read(device, S2_PLL_STATUS_A8) | S2_PLL_BYPASS);
	snooze(10000);
	return B_OK;
}


// #pragma mark - DDR [F]


static status_t
ddr_wait_idle(fthd_device* device, int32 tries, bigtime_t interval)
{
	return s2_wait(device, S2_DDR_STATUS_2018, S2_DDR_STATUS_BUSY, false,
		tries, interval);
}


static status_t
init_ddr(fthd_device* device)
{
	const uint32 speed = device->ddr_speed;
	const uint32 model = device->ddr_model;

	uint16 command = gPCI->read_pci_config(device->pci.bus,
		device->pci.device, device->pci.function, PCI_command, 2);
	if ((command & 0x7) == 0) {
		FTHD_LOG("PCI command register 0x%04x: device not enabled\n",
			command);
		return B_IO_ERROR;
	}

	uint32 value = s2_read(device, S2_PLL_CTRL_9C);
	s2_write(device, S2_PLL_CTRL_9C, value & 0xfffffcff);
	s2_write(device, S2_PLL_CTRL_9C, value | 0x300);

	status_t status = init_pll(device);
	if (status != B_OK)
		return status;

	// PHY soft reset
	s2_write(device, S2_PLL_STATUS_A8, 0x281);
	s2_write(device, S2_PLL_CTRL_9C, 0xfffff);
	snooze(10000);
	s2_write(device, S2_PLL_CTRL_9C, 0xffbff);

	s2_write(device, S2_DDR40_WL_DRV_PAD_CTL, 0x2);
	s2_write(device, S2_DDR40_WL_CLK_PAD_DISABLE, 0x2);
	s2_write(device, S2_20F8, 0x3f4);
		// no hardware frequency changes

	// DDR PLL, in two stages
	s2_write(device, 0x2434, 0x40);
	s2_write(device, 0x2438, 0x10000000);
	s2_write(device, 0x2424, 0x4);
	s2_write(device, 0x2430, 0x1f37291);
	if (s2_wait(device, S2_DDR_PLL_STATUS_2444, S2_DDR_PLL_2444_LOCKED, true,
			10001, 10) != B_OK) {
		FTHD_LOG("DDR PLL stage 1 did not lock\n");
		return B_IO_ERROR;
	}
	s2_write(device, 0x2430, 0x1f37205);
	if (s2_wait(device, S2_DDR_PLL_STATUS_241C, S2_DDR_PLL_241C_LOCKED, true,
			10001, 10) != B_OK) {
		FTHD_LOG("DDR PLL stage 2 did not lock\n");
		return B_IO_ERROR;
	}
	snooze(10000);

	// PHY PLL on safe settings
	s2_write(device, S2_DDR40_PHY_PLL_DIV, 0x0c10);
	s2_write(device, S2_DDR40_PHY_PLL_CFG, 0x0010);
	if (s2_wait(device, S2_DDR40_PHY_PLL_STATUS, S2_DDR40_PHY_PLL_LOCKED,
			true, 10001, 10) != B_OK) {
		FTHD_LOG("DDR PHY PLL did not lock\n");
		return B_IO_ERROR;
	}

	s2_write(device, S2_DDR_20A0, 0x10737545);
	s2_write(device, S2_DDR_20A4, 0x12643173);
	s2_write(device, S2_DDR_20A8, 0xff3f);
	s2_write(device, S2_DDR_20B0, model == 4 ? 0x46a00c2 : 0x42500c2);
	s2_write(device, S2_DDR_2118, 0x101f);
	s2_write(device, S2_DDR40_PHY_AUX_CTL, 0x1c0);

	// straps
	s2_write(device, S2_DDR40_STRAP_CTL, model == 4 ? 0x2159518 : 0x2155558);
	s2_write(device, S2_DDR40_STRAP_CTL_2,
		speed == 450 ? 0x108307 : 0x108286);
	s2_write(device, S2_DDR40_STRAP_CTL, 0x2159559);
	if (s2_wait(device, S2_DDR40_STRAP_STATUS, 1, true, 10000, 10) != B_OK) {
		FTHD_LOG("DDR straps not taken\n");
		return B_DEVICE_NOT_FOUND;
	}

	// PHY PLL on the real speed
	value = speed * 2 < 500 ? 0x2040 : 0x810;
	s2_write(device, S2_DDR40_PHY_PLL_DIV,
		(s2_read(device, S2_DDR40_PHY_PLL_DIV) & 0xffffc700) | value);
	s2_write(device, S2_DDR40_PHY_PLL_CFG,
		s2_read(device, S2_DDR40_PHY_PLL_CFG) & ~(uint32)0x2);
	if (s2_wait(device, S2_DDR40_PHY_PLL_STATUS, S2_DDR40_PHY_PLL_LOCKED,
			true, 100, 1) != B_OK) {
		FTHD_LOG("DDR PHY PLL did not lock on %" B_PRIu32 " MHz\n", speed);
		return B_DEVICE_NOT_FOUND;
	}

	// VDL calibration
	s2_write(device, S2_DDR40_PHY_VDL_CTL, 0);
	s2_write(device, S2_DDR40_PHY_VDL_CTL, 0x103);
	s2_wait(device, S2_DDR40_PHY_VDL_STATUS, 1, true, 100, 1);
	value = s2_read(device, S2_DDR40_PHY_VDL_STATUS);
	FTHD_LOG("VDL first pass: done %d, locked %d\n", (int)(value & 1),
		(int)((value >> 1) & 1));

	s2_write(device, S2_DDR40_PHY_VDL_CTL, 0);
	s2_write(device, S2_DDR40_PHY_VDL_CTL, 0);
	s2_write(device, S2_DDR40_PHY_VDL_CTL, 0x200);
	s2_wait(device, S2_DDR40_PHY_VDL_STATUS, 1, true, 1000, 1);
	value = s2_read(device, S2_DDR40_PHY_VDL_STATUS);

	uint32 stepSize;
	if ((value & 2) != 0)
		stepSize = (value & 0x0ffc) >> 2;
	else {
		// in 32 bit arithmetic, as [F] does it
		stepSize = ((1000000 / speed) * 0x4ec4ec4fu) >> 22;
	}

	uint32 fine = s2_read(device, S2_DDR40_PHY_VDL_CHAN_STATUS);
	if ((fine & 2) == 0) {
		fine = ((fine >> 8) & 0x3f) | 0x10100;
		s2_write(device, S2_DDR40_PHY_VDL_OVR_FINE, fine);

		uint32 coarse = 0x10000;
		stepSize >>= 4;
		stepSize += stepSize * 2;
		if (stepSize > 10) {
			stepSize = (stepSize + 118) >> 1;
			coarse = (stepSize & 0x3f) | 0x10000;
		}
		s2_write(device, S2_DDR40_PHY_VDL_OVR_COARSE, coarse);
	}
	FTHD_LOG("VDL status 0x%08" B_PRIx32 ", channel 0x%08" B_PRIx32 "\n",
		value, s2_read(device, S2_DDR40_PHY_VDL_CHAN_STATUS));

	// VTT, ZQ and pads
	s2_write(device, S2_DDR40_PHY_VTT_CONNECTIONS, 0x1cf7fff);
	s2_write(device, S2_DDR40_PHY_VTT_OVERRIDE, 0x77fff);
	s2_write(device, S2_DDR40_PHY_VTT_CTL, 0x4);
	s2_write(device, S2_DDR40_PHY_ZQ_PVT_COMP_CTL, 0xc0fff);
	s2_write(device, S2_DDR40_PHY_DRV_PAD_CTL, 0x2);
	s2_write(device, S2_DDR40_WL_DRV_PAD_CTL, 0x2);

	// read data delay, by clock period
	uint32 period = 1000000 / speed;
	uint32 delay = 4;
	if (period >= 400)
		delay = period > 900 ? 6 : 9;
	s2_write(device, S2_DDR40_WL_RD_DATA_DLY, delay);
	s2_write(device, S2_DDR40_WL_READ_CTL, 0x2);
	s2_write(device, S2_DDR40_WL_WR_PREAMBLE_MODE, 0x3);
	s2_write(device, S2_DDR40_WL_IDLE_PAD_CTL,
		s2_read(device, S2_DDR40_WL_IDLE_PAD_CTL) & 0xff0fffff);
	spin(500);

	s2_write(device, S2_DDR_2004, 0);
	snooze(10000);
	s2_write(device, S2_DDR_2014, 0xab0a);
	if (ddr_wait_idle(device, 10000, 10) != B_OK) {
		FTHD_LOG("DDR controller stays busy\n");
		return B_BUSY;
	}
	snooze(10000);

	s2_write(device, S2_3204, 0);
	FTHD_LOG("DDR strap status 0x%08" B_PRIx32 "\n",
		s2_read(device, S2_DDR40_STRAP_STATUS));
	s2_write(device, S2_3208, model == 4 ? 0x1fffffff : 0x0fffffff);
	s2_write(device, S2_3200, 0x1040);

	// mode registers
	const uint32 kModeWrites[] = {
		0x02000802, 0x3, 0x1, speed == 450 ? 0x17003000u : 0x17002000u
	};
	for (size_t i = 0; i < B_COUNT_OF(kModeWrites); i++) {
		if (ddr_wait_idle(device, 500, 5) != B_OK) {
			FTHD_LOG("DDR controller busy before mode write %d\n", (int)i);
			return B_BUSY;
		}
		s2_write(device, S2_DDR_2014, kModeWrites[i]);
	}

	s2_write(device, S2_DDR_2014, 0x20000);
	s2_write(device, S2_DDR_2008, 1);
	return B_OK;
}


// #pragma mark - memory check


static uint32
next_pattern(uint32& state)
{
	// xorshift32: any pattern that is not regular does
	state ^= state << 13;
	state ^= state >> 17;
	state ^= state << 5;
	return state;
}


/*!	Writes a pattern to a block of words at the start of the camera's
	memory, and to one word every megabyte across the rest of it, then reads
	it back. Returns the bits that came back wrong.
*/
static uint32
check_memory(fthd_device* device, uint32* _wrongWords, uint32* _words)
{
	const uint32 kDenseWords = 1024;
	const uint32 kStride = 1024 * 1024;

	uint32 state = 0x12345678;
	for (uint32 i = 0; i < kDenseWords; i++)
		mem_write(device, i * 4, next_pattern(state));
	for (uint32 offset = kStride; offset + 4 <= device->s2_mem_size;
			offset += kStride) {
		mem_write(device, offset, next_pattern(state));
	}

	uint32 wrongBits = 0;
	uint32 wrongWords = 0;
	uint32 words = 0;
	state = 0x12345678;
	for (uint32 i = 0; i < kDenseWords; i++, words++) {
		uint32 difference = mem_read(device, i * 4) ^ next_pattern(state);
		wrongBits |= difference;
		if (difference != 0)
			wrongWords++;
	}
	for (uint32 offset = kStride; offset + 4 <= device->s2_mem_size;
			offset += kStride, words++) {
		uint32 difference = mem_read(device, offset) ^ next_pattern(state);
		wrongBits |= difference;
		if (difference != 0)
			wrongWords++;
	}

	*_wrongWords = wrongWords;
	*_words = words;
	return wrongBits;
}


// #pragma mark - bring-up


status_t
fthd_hardware_init(fthd_device* device)
{
	// The Linux driver uses DDR model 4 at 450 MHz for every machine [F].
	device->ddr_model = 4;
	device->ddr_speed = 450;

	// no host pages mapped for the camera yet
	for (uint32 i = 0; i < S2_PAGE_TABLE_ENTRIES; i++)
		*(volatile uint32*)(device->s2_io + S2_PAGE_TABLE + i * 4) = 0;
	pci_post(device);

	status_t status = init_pcie_link(device);
	if (status != B_OK)
		return status;
	FTHD_LOG("S2 link up\n");

	for (uint32 offset = 0x1100; offset <= 0x111c; offset += 4)
		s2_write(device, offset, 0x203);

	status = init_ddr(device);
	if (status != B_OK) {
		FTHD_LOG("DDR set-up failed: %s\n", strerror(status));
		return status;
	}
	FTHD_LOG("DDR set up, model %" B_PRIu32 ", %" B_PRIu32 " MHz\n",
		device->ddr_model, device->ddr_speed);

	uint32 wrongWords;
	uint32 words;
	uint32 wrongBits = check_memory(device, &wrongWords, &words);
	if (wrongBits != 0) {
		FTHD_LOG("memory check failed: %" B_PRIu32 " of %" B_PRIu32
			" words wrong, bits 0x%08" B_PRIx32 "\n", wrongWords, words,
			wrongBits);
		return B_IO_ERROR;
	}
	FTHD_LOG("memory check passed: %" B_PRIu32 " words over %" B_PRIuSIZE
		" MB\n", words, device->s2_mem_size / (1024 * 1024));

	s2_write(device, S2_PCIE_D108, 0x8);
	s2_write(device, S2_PCIE_D104, 0xc);
	isp_write(device, ISP_REG_40004, 0);

	device->hardware_up = true;
	return B_OK;
}
