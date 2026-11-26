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
 * The chip side that does not depend on the bus: the cores found through the
 * EROM, resetting them, halting and starting the ARM CPU, and the size and
 * place of the RAM. The bus supplies backplane access through BrcmfBackplane.
 *
 * Follows OpenBSD's bwfm(4) (sys/dev/ic/bwfm.c 1.118, bwfmreg.h); moved
 * here from brcmf.cpp, where Bryan Irvine wrote it for the SDIO chips, so
 * that SDIO and PCIe share it. Handles ARM CR4/CA7 chips with TCM RAM and
 * ARM CM3 chips with SOCRAM.
 */
#ifndef BRCMF_CHIP_H
#define BRCMF_CHIP_H


#include <OS.h>


// backplane
enum {
	BRCMF_CHIP_BASE				= 0x18000000,
	BRCMF_CHIP_REG_CHIPID		= 0x000,
	BRCMF_CHIP_REG_CAPS			= 0x004,
	BRCMF_CHIP_REG_WATCHDOG		= 0x080,
	BRCMF_CHIP_REG_CAPS_EXT		= 0x0ac,
	BRCMF_CHIP_REG_EROMPTR		= 0x0fc,
	BRCMF_CHIP_REG_PMUCAPS		= 0x604,

	BRCMF_CAPS_PMU				= 0x10000000,
	BRCMF_CAPS_EXT_AOB			= 0x00000040,

	BRCMF_AGENT_IOCTL			= 0x408,
		BRCMF_IOCTL_CLK				= 0x0001,
		BRCMF_IOCTL_FGC				= 0x0002,
		BRCMF_IOCTL_D11_PHYCLOCKEN	= 0x0004,
		BRCMF_IOCTL_D11_PHYRESET	= 0x0008,
		BRCMF_IOCTL_CR4_CPUHALT		= 0x0020,
	BRCMF_AGENT_RESET_CTL		= 0x800,
		BRCMF_RESET_CTL_RESET		= 0x0001,

	BRCMF_CORE_CHIPCOMMON		= 0x800,
	BRCMF_CORE_INTERNAL_MEM		= 0x80e,
	BRCMF_CORE_80211			= 0x812,
	BRCMF_CORE_PMU				= 0x827,
	BRCMF_CORE_SDIO_DEV			= 0x829,
	BRCMF_CORE_ARM_CM3			= 0x82a,
	BRCMF_CORE_PCIE2			= 0x83c,
	BRCMF_CORE_ARM_CR4			= 0x83e,
	BRCMF_CORE_GCI				= 0x840,
	BRCMF_CORE_ARM_CA7			= 0x847,
	BRCMF_CORE_SYS_MEM			= 0x849,

	BRCMF_CR4_CAP				= 0x04,
	BRCMF_CR4_BANKIDX			= 0x40,
	BRCMF_CR4_BANKINFO			= 0x44,
	BRCMF_CR4_BANKPDA			= 0x4c,

	BRCMF_SOCRAM_COREINFO		= 0x00,
	BRCMF_SOCRAM_BANKIDX		= 0x10,
	BRCMF_SOCRAM_BANKINFO		= 0x40,
	BRCMF_SOCRAM_BANKPDA		= 0x44,
};


struct brcmf_chip_core {
	uint16		id;
	uint8		rev;
	uint32		base;
	uint32		wrap;
};


/*!	Backplane access as the bus gives it. */
class BrcmfBackplane {
public:
	virtual						~BrcmfBackplane() {}

	virtual	uint32				Read4(uint32 address) = 0;
	virtual	void				Write4(uint32 address, uint32 value) = 0;

	// Called after the cores are known and the CPU halted, to reset the
	// bus core side (PCIe: watchdog reset). Returns whether it did; the CPU
	// is then halted again.
	virtual	bool				ResetBus() { return false; }
	// Called just before the CPU is released: puts the reset vector where
	// an ARM CR4 starts (RAM address 0; \a vector is 0 for a CM3, which
	// starts from its own), and clears what the bus needs cleared.
	virtual	void				WriteResetVector(uint32 vector) = 0;
};


#define BRCMF_CHIP_MAX_CORES	32


class BrcmfChip {
public:
								BrcmfChip();

			status_t			Attach(BrcmfBackplane* backplane);

			uint16				Chip() const { return fChip; }
			uint8				ChipRev() const { return fChipRev; }
			const char*			Name() const { return fName; }
			uint32				RamBase() const { return fRamBase; }
			uint32				RamSize() const { return fRamSize; }
			void				SetRamSize(uint32 size) { fRamSize = size; }
			uint32				CcCaps() const { return fCcCaps; }
			uint32				CcCapsExt() const { return fCcCapsExt; }
			const brcmf_chip_core* Pmu() const;

			const brcmf_chip_core* Core(uint16 id, int index = 0) const;
			int32				CountCores() const { return fCoreCount; }
			const brcmf_chip_core* CoreAt(int32 index) const
									{ return &fCores[index]; }

			bool				CoreIsUp(const brcmf_chip_core* core);
			void				CoreDisable(const brcmf_chip_core* core,
									uint32 prereset, uint32 reset);
			void				CoreReset(const brcmf_chip_core* core,
									uint32 prereset, uint32 reset,
									uint32 postreset);

			void				SetPassive();
			status_t			SetActive(uint32 resetVector);

private:
			uint32				_Read4(uint32 address)
									{ return fBackplane->Read4(address); }
			void				_Write4(uint32 address, uint32 value)
									{ fBackplane->Write4(address, value); }

			void				_EromScan();
			bool				_EromGetRegAddress(uint32& erom,
									uint32& base, uint32& wrap);
			status_t			_RamInfo();
			status_t			_SocramInfo();

private:
			BrcmfBackplane*		fBackplane;
			uint16				fChip;
			uint8				fChipRev;
			char				fName[8];
			brcmf_chip_core		fCores[BRCMF_CHIP_MAX_CORES];
			int32				fCoreCount;
			uint32				fRamBase;
			uint32				fRamSize;
			uint32				fCcCaps;
			uint32				fCcCapsExt;
};


#endif	// BRCMF_CHIP_H
