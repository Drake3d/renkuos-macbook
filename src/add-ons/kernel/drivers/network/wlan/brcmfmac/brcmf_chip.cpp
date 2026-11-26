/*
 * Copyright 2026, Bryan Irvine
 * Copyright 2026, RenkuOS Contributors
 * Copyright (c) 2010-2016 Broadcom Corporation
 * Copyright (c) 2016,2017 Patrick Wildt <patrick@blueri.se>
 * SPDX-License-Identifier: ISC
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Bus independent chip handling for Broadcom FullMAC chips.
 *
 * Provenance: follows OpenBSD's bwfm(4) chip code (sys/dev/ic/bwfm.c:
 * bwfm_chip_attach(), the AI core reset, the DMP EROM scan, the CR4
 * passive/active and TCM RAM size functions), as already written for the
 * SDIO chips in brcmf.cpp by Bryan Irvine. Linux brcmfmac (ISC) compared
 * only.
 */


#include "brcmf_chip.h"

#include <KernelExport.h>
#include <stdio.h>
#include <string.h>

#include "brcmfmac.h"


// EROM descriptors
enum {
	DMP_DESC_MASK			= 0x0000000f,
	DMP_DESC_COMPONENT		= 0x00000001,
	DMP_DESC_MASTER_PORT	= 0x00000003,
	DMP_DESC_ADDRESS		= 0x00000005,
	DMP_DESC_ADDRSIZE_GT32	= 0x00000008,
	DMP_DESC_EOT			= 0x0000000f,
	DMP_COMP_PARTNUM		= 0x000fff00,
	DMP_COMP_PARTNUM_S		= 8,
	DMP_COMP_REVISION		= 0xff000000,
	DMP_COMP_REVISION_S		= 24,
	DMP_COMP_NUM_SWRAP		= 0x00f80000,
	DMP_COMP_NUM_SWRAP_S	= 19,
	DMP_COMP_NUM_MWRAP		= 0x0007c000,
	DMP_COMP_NUM_MWRAP_S	= 14,
	DMP_SLAVE_ADDR_BASE		= 0xfffff000,
	DMP_SLAVE_TYPE			= 0x000000c0,
	DMP_SLAVE_TYPE_S		= 6,
	DMP_SLAVE_TYPE_SLAVE	= 0,
	DMP_SLAVE_TYPE_SWRAP	= 2,
	DMP_SLAVE_TYPE_MWRAP	= 3,
	DMP_SLAVE_SIZE_TYPE		= 0x00000030,
	DMP_SLAVE_SIZE_TYPE_S	= 4,
	DMP_SLAVE_SIZE_4K		= 0,
	DMP_SLAVE_SIZE_8K		= 1,
	DMP_SLAVE_SIZE_DESC		= 3,
};


BrcmfChip::BrcmfChip()
	:
	fBackplane(NULL),
	fChip(0),
	fChipRev(0),
	fCoreCount(0),
	fRamBase(0),
	fRamSize(0),
	fCcCaps(0),
	fCcCapsExt(0)
{
	fName[0] = '\0';
}


const brcmf_chip_core*
BrcmfChip::Core(uint16 id, int index) const
{
	for (int32 i = 0; i < fCoreCount; i++) {
		if (fCores[i].id == id && index-- == 0)
			return &fCores[i];
	}
	return NULL;
}


bool
BrcmfChip::CoreIsUp(const brcmf_chip_core* core)
{
	uint32 ioctl = _Read4(core->wrap + BRCMF_AGENT_IOCTL);
	uint32 reset = _Read4(core->wrap + BRCMF_AGENT_RESET_CTL);
	return (ioctl & (BRCMF_IOCTL_FGC | BRCMF_IOCTL_CLK)) == BRCMF_IOCTL_CLK
		&& (reset & BRCMF_RESET_CTL_RESET) == 0;
}


void
BrcmfChip::CoreDisable(const brcmf_chip_core* core, uint32 prereset,
	uint32 reset)
{
	uint32 value = _Read4(core->wrap + BRCMF_AGENT_RESET_CTL);
	if ((value & BRCMF_RESET_CTL_RESET) == 0) {
		_Write4(core->wrap + BRCMF_AGENT_IOCTL,
			prereset | BRCMF_IOCTL_FGC | BRCMF_IOCTL_CLK);
		_Read4(core->wrap + BRCMF_AGENT_IOCTL);

		_Write4(core->wrap + BRCMF_AGENT_RESET_CTL, BRCMF_RESET_CTL_RESET);
		spin(20);

		int i;
		for (i = 300; i > 0; i--) {
			if (_Read4(core->wrap + BRCMF_AGENT_RESET_CTL)
					== BRCMF_RESET_CTL_RESET) {
				break;
			}
		}
		if (i == 0)
			ERROR("timeout on core %#x reset\n", core->id);
	}

	_Write4(core->wrap + BRCMF_AGENT_IOCTL,
		reset | BRCMF_IOCTL_FGC | BRCMF_IOCTL_CLK);
	_Read4(core->wrap + BRCMF_AGENT_IOCTL);
}


void
BrcmfChip::CoreReset(const brcmf_chip_core* core, uint32 prereset,
	uint32 reset, uint32 postreset)
{
	CoreDisable(core, prereset, reset);

	int i;
	for (i = 50; i > 0; i--) {
		if ((_Read4(core->wrap + BRCMF_AGENT_RESET_CTL)
				& BRCMF_RESET_CTL_RESET) == 0) {
			break;
		}
		_Write4(core->wrap + BRCMF_AGENT_RESET_CTL, 0);
		spin(60);
	}
	if (i == 0)
		ERROR("timeout on core %#x reset release\n", core->id);

	_Write4(core->wrap + BRCMF_AGENT_IOCTL, postreset | BRCMF_IOCTL_CLK);
	_Read4(core->wrap + BRCMF_AGENT_IOCTL);
}


bool
BrcmfChip::_EromGetRegAddress(uint32& erom, uint32& base, uint32& wrap)
{
	base = 0;
	wrap = 0;

	uint32 value = _Read4(erom);
	uint32 type = value & DMP_DESC_MASK;
	uint32 wrapType;
	if (type == DMP_DESC_MASTER_PORT) {
		wrapType = DMP_SLAVE_TYPE_MWRAP;
		erom += 4;
	} else if ((type & ~DMP_DESC_ADDRSIZE_GT32) == DMP_DESC_ADDRESS)
		wrapType = DMP_SLAVE_TYPE_SWRAP;
	else
		return false;

	int guard = 0;
	do {
		do {
			value = _Read4(erom);
			type = value & DMP_DESC_MASK;
			if (type == DMP_DESC_COMPONENT)
				return true;
			if (type == DMP_DESC_EOT)
				return false;
			erom += 4;
			if (++guard > 512)
				return false;
		} while ((type & ~DMP_DESC_ADDRSIZE_GT32) != DMP_DESC_ADDRESS);

		if ((type & DMP_DESC_ADDRSIZE_GT32) != 0)
			erom += 4;

		uint32 sizeType = (value & DMP_SLAVE_SIZE_TYPE)
			>> DMP_SLAVE_SIZE_TYPE_S;
		if (sizeType == DMP_SLAVE_SIZE_DESC) {
			uint32 size = _Read4(erom);
			if ((size & DMP_DESC_ADDRSIZE_GT32) != 0)
				erom += 8;
			else
				erom += 4;
		}
		if (sizeType != DMP_SLAVE_SIZE_4K && sizeType != DMP_SLAVE_SIZE_8K)
			continue;

		uint32 slaveType = (value & DMP_SLAVE_TYPE) >> DMP_SLAVE_TYPE_S;
		if (base == 0 && slaveType == DMP_SLAVE_TYPE_SLAVE)
			base = value & DMP_SLAVE_ADDR_BASE;
		if (wrap == 0 && slaveType == wrapType)
			wrap = value & DMP_SLAVE_ADDR_BASE;
	} while (base == 0 || wrap == 0);

	return true;
}


void
BrcmfChip::_EromScan()
{
	uint32 erom = _Read4(BRCMF_CHIP_BASE + BRCMF_CHIP_REG_EROMPTR);
	uint32 type = 0;
	int guard = 0;

	while (type != DMP_DESC_EOT && fCoreCount < BRCMF_CHIP_MAX_CORES
		&& ++guard < 1024) {
		uint32 value = _Read4(erom);
		type = value & DMP_DESC_MASK;
		erom += 4;

		if (type != DMP_DESC_COMPONENT)
			continue;

		uint16 id = (value & DMP_COMP_PARTNUM) >> DMP_COMP_PARTNUM_S;

		value = _Read4(erom);
		type = value & DMP_DESC_MASK;
		erom += 4;
		if (type != DMP_DESC_COMPONENT) {
			ERROR("EROM: not a component descriptor\n");
			return;
		}

		uint32 masterWraps = (value & DMP_COMP_NUM_MWRAP)
			>> DMP_COMP_NUM_MWRAP_S;
		uint32 slaveWraps = (value & DMP_COMP_NUM_SWRAP)
			>> DMP_COMP_NUM_SWRAP_S;
		uint8 revision = (value & DMP_COMP_REVISION) >> DMP_COMP_REVISION_S;

		if (masterWraps + slaveWraps == 0 && id != BRCMF_CORE_PMU
			&& id != BRCMF_CORE_GCI) {
			continue;
		}

		uint32 base, wrap;
		if (!_EromGetRegAddress(erom, base, wrap))
			continue;

		brcmf_chip_core& core = fCores[fCoreCount++];
		core.id = id;
		core.rev = revision;
		core.base = base;
		core.wrap = wrap;
	}
}


/*!	Halts the ARM CR4 and holds the 802.11 cores in reset, ready for a
	firmware download. Only the CR4 (the CPU of the PCIe chips this is used
	for) is handled.
*/
void
BrcmfChip::SetPassive()
{
	const brcmf_chip_core* core = Core(BRCMF_CORE_ARM_CR4);
	if (core == NULL)
		return;

	uint32 value = _Read4(core->wrap + BRCMF_AGENT_IOCTL);
	CoreReset(core, value & BRCMF_IOCTL_CR4_CPUHALT, BRCMF_IOCTL_CR4_CPUHALT,
		BRCMF_IOCTL_CR4_CPUHALT);

	const brcmf_chip_core* d11;
	for (int i = 0; (d11 = Core(BRCMF_CORE_80211, i)) != NULL; i++) {
		CoreDisable(d11, BRCMF_IOCTL_D11_PHYRESET | BRCMF_IOCTL_D11_PHYCLOCKEN,
			BRCMF_IOCTL_D11_PHYCLOCKEN);
	}
}


/*!	Starts the ARM CR4 at \a resetVector (the first word of the firmware). */
status_t
BrcmfChip::SetActive(uint32 resetVector)
{
	const brcmf_chip_core* core = Core(BRCMF_CORE_ARM_CR4);
	if (core == NULL)
		return B_NOT_SUPPORTED;

	fBackplane->WriteResetVector(resetVector);
	CoreReset(core, BRCMF_IOCTL_CR4_CPUHALT, 0, 0);
	return B_OK;
}


status_t
BrcmfChip::_RamInfo()
{
	const brcmf_chip_core* core = Core(BRCMF_CORE_ARM_CR4);
	if (core == NULL) {
		ERROR("no ARM CR4 core; only CR4 chips are supported on PCIe\n");
		return B_NOT_SUPPORTED;
	}

	uint32 cap = _Read4(core->base + BRCMF_CR4_CAP);
	uint32 banks = (cap & 0xf) + ((cap >> 4) & 0xf);
	uint32 size = 0;
	for (uint32 i = 0; i < banks; i++) {
		_Write4(core->base + BRCMF_CR4_BANKIDX, i);
		uint32 info = _Read4(core->base + BRCMF_CR4_BANKINFO);
		uint32 blockSize = (info & 0x200) != 0 ? 1024 : 8192;
		size += ((info & 0x7f) + 1) * blockSize;
	}
	fRamSize = size;

	switch (fChip) {
		case 0x4345:
			fRamBase = 0x198000;
			break;
		case 0x4335:
		case 0x4339:
		case 0x4350:
		case 0x4354:
		case 0x4356:
		case 43567:
		case 43569:
		case 43570:
		case 0x4358:
		case 43602:
		case 0x4371:
			fRamBase = 0x180000;
			break;
		default:
			ERROR("unknown RAM base for chip %s\n", fName);
			return B_NOT_SUPPORTED;
	}
	return B_OK;
}


status_t
BrcmfChip::Attach(BrcmfBackplane* backplane)
{
	fBackplane = backplane;
	fCoreCount = 0;

	uint32 id = _Read4(BRCMF_CHIP_BASE + BRCMF_CHIP_REG_CHIPID);
	if (id == 0xffffffff) {
		ERROR("chip does not answer (chip ID reads all ones)\n");
		return B_DEVICE_NOT_FOUND;
	}
	fChip = id & 0xffff;
	fChipRev = (id >> 16) & 0xf;
	uint32 interconnect = (id >> 28) & 0xf;
	if (fChip > 0xa000 || fChip < 0x4000)
		snprintf(fName, sizeof(fName), "%u", fChip);
	else
		snprintf(fName, sizeof(fName), "%x", fChip);
	INFO("chip BCM%s revision %u, interconnect %" B_PRIu32 "\n", fName,
		fChipRev, interconnect);
	if (interconnect != 1) {
		ERROR("unsupported interconnect type %" B_PRIu32 "\n", interconnect);
		return B_NOT_SUPPORTED;
	}

	_EromScan();

	bool cpuFound = false;
	for (int32 i = 0; i < fCoreCount; i++) {
		INFO("core %#x rev %u base %#" B_PRIx32 " wrap %#" B_PRIx32 "\n",
			fCores[i].id, fCores[i].rev, fCores[i].base, fCores[i].wrap);
		switch (fCores[i].id) {
			case BRCMF_CORE_ARM_CM3:
			case BRCMF_CORE_ARM_CR4:
			case BRCMF_CORE_ARM_CA7:
				cpuFound = true;
				break;
		}
	}
	if (!cpuFound) {
		ERROR("CPU core not found\n");
		return B_ERROR;
	}

	SetPassive();
	fBackplane->ResetBus();
	SetPassive();

	status_t status = _RamInfo();
	if (status != B_OK)
		return status;

	const brcmf_chip_core* cc = Core(BRCMF_CORE_CHIPCOMMON);
	if (cc == NULL) {
		ERROR("chipcommon core not found\n");
		return B_ERROR;
	}
	fCcCaps = _Read4(cc->base + BRCMF_CHIP_REG_CAPS);
	fCcCapsExt = _Read4(cc->base + BRCMF_CHIP_REG_CAPS_EXT);

	INFO("RAM %" B_PRIu32 " KB at %#" B_PRIx32 "\n", fRamSize / 1024,
		fRamBase);
	return B_OK;
}
