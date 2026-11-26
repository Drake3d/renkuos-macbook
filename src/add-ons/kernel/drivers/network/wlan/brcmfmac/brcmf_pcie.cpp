/*
 * Copyright 2026, RenkuOS Contributors
 * Copyright (c) 2010-2016 Broadcom Corporation
 * Copyright (c) 2017 Patrick Wildt <patrick@blueri.se>
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
 * PCIe bring-up for Broadcom FullMAC chips.
 *
 * Provenance: the register layout, the backplane window, the bus core
 * reset, the firmware names, the firmware download and the shared area
 * follow OpenBSD's bwfm(4) PCIe attachment (sys/dev/pci/if_bwfm_pci.c and
 * if_bwfm_pci.h: bwfm_pci_attach(), bwfm_pci_preinit(),
 * bwfm_pci_load_microcode(), bwfm_pci_buscore_*(), bwfm_pci_debug_console()),
 * Patrick Wildt and Broadcom Corporation, ISC. Ported-from: OpenBSD
 * if_bwfm_pci.c 1.79. Linux brcmfmac pcie.c (ISC) compared only.
 *
 * The firmware is Broadcom's and is not part of RenkuOS; it is read from
 * <data>/firmware/brcm/brcmfmac<chip>-pcie.bin (linux-firmware names).
 */


#include "brcmf_pcie.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <FindDirectory.h>
#include <KernelExport.h>

#include "brcmfmac.h"
#include "brcmf_pcie_regs.h"


// PCI configuration space
static const uint16 kConfigBar0Window = 0x80;
static const uint16 kConfigLinkStatusControl = 0xbc;
static const uint32 kLinkStatusControlAspm = 0x3;

// BAR0: a 4 KB window onto the backplane, and the PCIe core at 0x2000.
static const uint32 kBar0WindowSize = 0x1000;
static const uint32 kPcieCoreOffset = 0x2000;

// Configuration registers of the PCIe core that are written back after the
// watchdog reset on core revisions up to 13.
static const uint16 kConfigRestore[] = {
	0x004,	// status/command
	0x04c,	// PM CSR
	0x058,	// MSI capability
	0x05c,	// MSI address low
	0x060,	// MSI address high
	0x064,	// MSI data
	0x0dc,	// link status/control 2
	0x228,	// RBAR control
	0x248,	// PML1 sub control 1
	0x4e0,	// BAR2 config
	0x4f4,	// BAR3 config
};

// Firmware image
static const uint32 kFirmwareRamSizeOffset = 0x6c;
static const uint32 kFirmwareRamSizeMagic = 0x534d4152;	// "SMAR"

static const uint32 kSharedMinVersion = 5;
static const uint32 kSharedMaxVersion = 7;

static const size_t kMaxFirmwareSize = 4 * 1024 * 1024;


static status_t
read_firmware_file(const char* name, uint8** _data, size_t* _size)
{
	static const directory_which kDirectories[] = {
		B_SYSTEM_NONPACKAGED_DATA_DIRECTORY,
		B_SYSTEM_DATA_DIRECTORY,
	};
	static const char* kSubdirectories[] = { "brcm", "brcmfmac" };

	char path[B_PATH_NAME_LENGTH];
	int fd = -1;
	for (size_t i = 0; i < B_COUNT_OF(kDirectories) && fd < 0; i++) {
		char base[B_PATH_NAME_LENGTH];
		if (find_directory(kDirectories[i], -1, false, base, sizeof(base))
				!= B_OK) {
			continue;
		}
		for (size_t j = 0; j < B_COUNT_OF(kSubdirectories) && fd < 0; j++) {
			snprintf(path, sizeof(path), "%s/firmware/%s/%s", base,
				kSubdirectories[j], name);
			fd = open(path, O_RDONLY);
		}
	}
	if (fd < 0)
		return B_ENTRY_NOT_FOUND;

	off_t size = lseek(fd, 0, SEEK_END);
	lseek(fd, 0, SEEK_SET);
	if (size <= 0 || size > (off_t)kMaxFirmwareSize) {
		close(fd);
		return B_BAD_DATA;
	}

	uint8* data = (uint8*)malloc(size);
	if (data == NULL) {
		close(fd);
		return B_NO_MEMORY;
	}
	ssize_t bytesRead = read(fd, data, size);
	close(fd);
	if (bytesRead != size) {
		free(data);
		return B_IO_ERROR;
	}

	INFO("read %s (%" B_PRIdOFF " bytes)\n", path, size);
	*_data = data;
	*_size = size;
	return B_OK;
}


static inline uint32
get_le32(const uint8* data)
{
	return data[0] | (data[1] << 8) | (data[2] << 16)
		| ((uint32)data[3] << 24);
}


// #pragma mark -


BrcmfPcie::BrcmfPcie(pci_device_module_info* pci, pci_device* device)
	:
	fPci(pci),
	fDevice(device),
	fRegArea(-1),
	fRegs(NULL),
	fRegSize(0),
	fTcmArea(-1),
	fTcm(NULL),
	fTcmSize(0),
	fWindow(0xffffffff),
	fRegisters64(false),
	fRunning(false),
	fSharedAddress(0),
	fSharedFlags(0),
	fSharedVersion(0),
	fConsoleAddress(0),
	fConsoleBuffer(0),
	fConsoleSize(0),
	fConsoleRead(0),
	fConsoleBusy(0),
	fMsgbufReady(false),
	fRxDataOffset(0),
	fMaxRxBufPost(0),
	fHostToDeviceMailbox(0),
	fDeviceToHostMailbox(0),
	fMaxSubmissionRings(0),
	fMaxFlowRings(0),
	fMaxCompletionRings(0),
	fIoctlTransaction(0),
	fIoctlDone(false),
	fIoctlStatus(0),
	fIoctlReply(NULL),
	fIoctlReplyLength(0),
	fIrq(0),
	fMsi(false),
	fHandlerInstalled(false),
	fServiceSem(-1),
	fServiceThreadId(-1),
	fStopService(false),
	fPendingStatus(0),
	fInterruptCount(0),
	fPolledMessages(0),
	fEventCount(0)
{
	memset(&fInfo, 0, sizeof(fInfo));

	brcmf_dma_buffer empty = { -1, NULL, 0, 0 };
	brcmf_msgring* rings[] = { &fControlSubmit, &fRxPostSubmit,
		&fControlComplete, &fTxComplete, &fRxComplete };
	for (size_t i = 0; i < B_COUNT_OF(rings); i++) {
		memset(rings[i], 0, sizeof(brcmf_msgring));
		rings[i]->memory = empty;
	}
	fScratch = fRingUpdate = fIoctlRequest = empty;
	for (int i = 0; i < BRCMF_PCIE_CONTROL_BUFFERS; i++) {
		fControlBuffers[i] = empty;
		fControlPosted[i] = false;
	}

	mutex_init(&fRingLock, "brcmfmac pcie rings");
	mutex_init(&fIoctlLock, "brcmfmac pcie ioctl");
	fIoctlCondition.Init(this, "brcmfmac ioctl");
}


BrcmfPcie::~BrcmfPcie()
{
	Stop();
	mutex_destroy(&fRingLock);
	mutex_destroy(&fIoctlLock);
	if (fRegArea >= 0)
		delete_area(fRegArea);
	if (fTcmArea >= 0)
		delete_area(fTcmArea);
}


status_t
BrcmfPcie::_MapBar(int index, const char* name, area_id* _area,
	volatile uint8** _base, size_t* _size, int* _nextIndex)
{
	if (index > 5)
		return B_BAD_INDEX;

	phys_addr_t address = fInfo.u.h0.base_registers[index];
	size_t size = fInfo.u.h0.base_register_sizes[index];
	int next = index + 1;
	if ((fInfo.u.h0.base_register_flags[index] & PCI_address_type)
			== PCI_address_type_64 && index < 5) {
		address |= (uint64)fInfo.u.h0.base_registers[index + 1] << 32;
		next = index + 2;
	}
	if (_nextIndex != NULL)
		*_nextIndex = next;
	if (address == 0 || size == 0) {
		ERROR("BAR %d is not assigned\n", index);
		return B_DEVICE_NOT_FOUND;
	}

	void* base;
	area_id area = map_physical_memory(name, address, size,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		&base);
	if (area < 0) {
		ERROR("cannot map BAR %d: %s\n", index, strerror(area));
		return area;
	}

	INFO("BAR %d at %#" B_PRIxPHYSADDR ", %" B_PRIuSIZE " KB\n", index,
		address, size / 1024);
	*_area = area;
	*_base = (volatile uint8*)base;
	*_size = size;
	return B_OK;
}


status_t
BrcmfPcie::Init()
{
	fPci->get_pci_info(fDevice, &fInfo);
	INFO("PCIe card %04x:%04x revision %02x, subsystem %04x:%04x\n",
		fInfo.vendor_id, fInfo.device_id, fInfo.revision,
		fInfo.u.h0.subsystem_vendor_id, fInfo.u.h0.subsystem_id);

	// BAR0 holds the registers, the next BAR the chip's RAM (TCM).
	int next;
	status_t status = _MapBar(0, "brcmfmac registers", &fRegArea, &fRegs,
		&fRegSize, &next);
	if (status != B_OK)
		return status;
	status = _MapBar(next, "brcmfmac tcm", &fTcmArea, &fTcm, &fTcmSize,
		NULL);
	if (status != B_OK)
		return status;
	if (fRegSize < kPcieCoreOffset + kBar0WindowSize) {
		ERROR("register BAR too small\n");
		return B_BAD_DATA;
	}

	uint16 command = fPci->read_pci_config(fDevice, PCI_command, 2);
	command |= PCI_command_memory | PCI_command_master;
	fPci->write_pci_config(fDevice, PCI_command, 2, command);

	return B_OK;
}


// #pragma mark - register access


uint32
BrcmfPcie::_ConfigRead(uint16 offset)
{
	return fPci->read_pci_config(fDevice, offset, 4);
}


void
BrcmfPcie::_ConfigWrite(uint16 offset, uint32 value)
{
	fPci->write_pci_config(fDevice, offset, 4, value);
}


void
BrcmfPcie::_SetWindow(uint32 address)
{
	if (address == fWindow)
		return;
	_ConfigWrite(kConfigBar0Window, address);
	if (_ConfigRead(kConfigBar0Window) != address)
		_ConfigWrite(kConfigBar0Window, address);
	fWindow = address;
}


void
BrcmfPcie::_SelectCore(uint16 id)
{
	const brcmf_chip_core* core = fChip.Core(id);
	if (core == NULL) {
		ERROR("core %#x not found\n", id);
		return;
	}
	_SetWindow(core->base);
}


uint32
BrcmfPcie::_RegRead(uint32 offset)
{
	return *(volatile uint32*)(fRegs + offset);
}


void
BrcmfPcie::_RegWrite(uint32 offset, uint32 value)
{
	*(volatile uint32*)(fRegs + offset) = value;
}


uint32
BrcmfPcie::_PcieRead(uint32 offset)
{
	return _RegRead(kPcieCoreOffset + offset);
}


void
BrcmfPcie::_PcieWrite(uint32 offset, uint32 value)
{
	_RegWrite(kPcieCoreOffset + offset, value);
}


uint32
BrcmfPcie::_TcmRead4(uint32 address)
{
	if (address + 4 > fTcmSize)
		return 0xffffffff;
	return *(volatile uint32*)(fTcm + address);
}


void
BrcmfPcie::_TcmWrite4(uint32 address, uint32 value)
{
	if (address + 4 > fTcmSize)
		return;
	*(volatile uint32*)(fTcm + address) = value;
}


uint8
BrcmfPcie::_TcmRead1(uint32 address)
{
	if (address >= fTcmSize)
		return 0xff;
	return fTcm[address];
}


uint32
BrcmfPcie::Read4(uint32 address)
{
	_SetWindow(address & ~(kBar0WindowSize - 1));
	return _RegRead(address & (kBar0WindowSize - 1));
}


void
BrcmfPcie::Write4(uint32 address, uint32 value)
{
	_SetWindow(address & ~(kBar0WindowSize - 1));
	_RegWrite(address & (kBar0WindowSize - 1), value);
}


uint32
BrcmfPcie::_InterruptStatus()
{
	return _PcieRead(fRegisters64 ? PCIE2_64_MAILBOXINT : PCIE2_MAILBOXINT);
}


void
BrcmfPcie::_AckInterrupts(uint32 status)
{
	_PcieWrite(fRegisters64 ? PCIE2_64_MAILBOXINT : PCIE2_MAILBOXINT,
		status);
}


// #pragma mark - BrcmfBackplane hooks


/*!	Resets the chip through the chipcommon watchdog, with ASPM switched off
	meanwhile, and restores what the reset loses.
*/
void
BrcmfPcie::ResetBus()
{
	_SelectCore(BRCMF_CORE_PCIE2);
	uint32 link = _ConfigRead(kConfigLinkStatusControl);
	_ConfigWrite(kConfigLinkStatusControl, link & ~kLinkStatusControlAspm);

	const brcmf_chip_core* cc = fChip.Core(BRCMF_CORE_CHIPCOMMON);
	if (cc != NULL) {
		_SetWindow(cc->base);
		_RegWrite(BRCMF_CHIP_REG_WATCHDOG, 4);
		snooze(100000);
	}
	// the window register itself may have been reset
	fWindow = 0xffffffff;

	_SelectCore(BRCMF_CORE_PCIE2);
	_ConfigWrite(kConfigLinkStatusControl, link);

	const brcmf_chip_core* pcie = fChip.Core(BRCMF_CORE_PCIE2);
	if (pcie == NULL)
		return;
	if (pcie->rev <= 13) {
		for (size_t i = 0; i < B_COUNT_OF(kConfigRestore); i++) {
			_PcieWrite(PCIE2_CONFIGADDR, kConfigRestore[i]);
			uint32 value = _PcieRead(PCIE2_CONFIGDATA);
			_PcieWrite(PCIE2_CONFIGDATA, value);
		}
	}
	fRegisters64 = pcie->rev >= 64;

	uint32 status = _InterruptStatus();
	if (status != 0xffffffff)
		_AckInterrupts(status);
}


void
BrcmfPcie::WriteResetVector(uint32 vector)
{
	_TcmWrite4(0, vector);
}


// #pragma mark - bring-up


const char*
BrcmfPcie::_FirmwareName()
{
	switch (fChip.Chip()) {
		case 0x4350:
			return fChip.ChipRev() <= 7 ? "4350c2" : "4350";
		case 0x4356:
			return "4356";
		case 43602:
			return "43602";
		case 0x4371:
			return "4371";
		default:
			return NULL;
	}
}


status_t
BrcmfPcie::_LoadFirmware(const uint8* firmware, size_t size)
{
	uint32 ramBase = fChip.RamBase();
	uint32 ramSize = fChip.RamSize();
	if (ramBase + ramSize > fTcmSize || size > ramSize) {
		ERROR("firmware (%" B_PRIuSIZE " bytes) or RAM (%" B_PRIu32
			" at %#" B_PRIx32 ") does not fit the %" B_PRIuSIZE " KB window\n",
			size, ramSize, ramBase, fTcmSize / 1024);
		return B_BAD_DATA;
	}

	bigtime_t start = system_time();
	size_t aligned = size & ~(size_t)3;
	for (size_t i = 0; i < aligned; i += 4)
		_TcmWrite4(ramBase + i, get_le32(firmware + i));
	if (aligned < size) {
		uint8 tail[4] = {};
		memcpy(tail, firmware + aligned, size - aligned);
		_TcmWrite4(ramBase + aligned, get_le32(tail));
	}

	// check the copy
	for (size_t i = 0; i < aligned; i += 4) {
		if (_TcmRead4(ramBase + i) != get_le32(firmware + i)) {
			ERROR("firmware check failed at offset %#" B_PRIxSIZE "\n", i);
			return B_IO_ERROR;
		}
	}
	INFO("firmware copied in %" B_PRId64 " ms\n",
		(system_time() - start) / 1000);

	// The firmware replaces the last word of RAM with the address of the
	// shared area once it runs. No NVRAM file is used on PCIe for now (the
	// MacBook's chip does without one).
	uint32 last = ramBase + ramSize - 4;
	_TcmWrite4(last, 0);
	uint32 written = _TcmRead4(last);

	status_t status = fChip.SetActive(get_le32(firmware));
	if (status != B_OK)
		return status;

	uint32 shared = written;
	for (int i = 0; i < 100 && shared == written; i++) {
		snooze(50000);
		shared = _TcmRead4(last);
	}
	if (shared == written) {
		ERROR("firmware did not come up\n");
		return B_TIMED_OUT;
	}
	if (shared < ramBase || shared >= ramBase + ramSize) {
		ERROR("invalid shared area address %#" B_PRIx32 "\n", shared);
		return B_BAD_DATA;
	}

	INFO("firmware running after %" B_PRId64 " ms, shared area at %#"
		B_PRIx32 "\n", (system_time() - start) / 1000, shared);
	fSharedAddress = shared;
	return B_OK;
}


void
BrcmfPcie::_ReadShared()
{
	fSharedFlags = _TcmRead4(fSharedAddress + SHARED_INFO);
	fSharedVersion = fSharedFlags & SHARED_INFO_VERSION_MASK;

	uint32 maxRxPost = _TcmRead4(fSharedAddress + 0x20) >> 16;
	uint32 rxDataOffset = _TcmRead4(fSharedAddress + SHARED_RX_DATAOFFSET);
	uint32 ringInfo = _TcmRead4(fSharedAddress + SHARED_RING_INFO_ADDR);
	fConsoleAddress = _TcmRead4(fSharedAddress + SHARED_CONSOLE_ADDR);

	INFO("shared area: version %" B_PRIu32 ", flags %#" B_PRIx32
		"%s%s%s%s\n", fSharedVersion, fSharedFlags,
		(fSharedFlags & SHARED_INFO_DMA_INDEX) != 0 ? ", DMA indices" : "",
		(fSharedFlags & SHARED_INFO_DMA_2B_IDX) != 0 ? " (2 bytes)" : "",
		(fSharedFlags & SHARED_INFO_USE_MAILBOX) != 0 ? ", mailbox" : "",
		(fSharedFlags & SHARED_INFO_HOSTRDY_DB1) != 0 ? ", host ready DB1"
			: "");
	INFO("shared area: max RX buffers %" B_PRIu32 ", RX data offset %"
		B_PRIu32 ", ring info at %#" B_PRIx32 ", console at %#" B_PRIx32
		"\n", maxRxPost, rxDataOffset, ringInfo, fConsoleAddress);

	if (fSharedVersion < kSharedMinVersion
		|| fSharedVersion > kSharedMaxVersion) {
		ERROR("shared area version %" B_PRIu32 " is not supported (%"
			B_PRIu32 "-%" B_PRIu32 ")\n", fSharedVersion, kSharedMinVersion,
			kSharedMaxVersion);
	}

	uint32 ramBase = fChip.RamBase();
	uint32 ramEnd = ramBase + fChip.RamSize();
	if (fConsoleAddress >= ramBase && fConsoleAddress < ramEnd) {
		fConsoleBuffer = _TcmRead4(fConsoleAddress + CONSOLE_BUFADDR);
		fConsoleSize = _TcmRead4(fConsoleAddress + CONSOLE_BUFSIZE);
		if (fConsoleBuffer < ramBase || fConsoleSize == 0
			|| fConsoleSize > 0x10000
			|| fConsoleBuffer + fConsoleSize > ramEnd) {
			fConsoleBuffer = 0;
			fConsoleSize = 0;
		}
	}
}


/*!	Copies what the firmware printed on its console since the last call to
	the syslog, a line at a time.
*/
void
BrcmfPcie::_DumpConsole()
{
	if (fConsoleSize == 0)
		return;
	// called from the service thread and from start-up and stop
	if (atomic_test_and_set(&fConsoleBusy, 1, 0) != 0)
		return;

	uint32 writeIndex = _TcmRead4(fConsoleAddress + CONSOLE_WRITEIDX);
	if (writeIndex >= fConsoleSize) {
		atomic_set(&fConsoleBusy, 0);
		return;
	}

	char line[160];
	size_t length = 0;
	uint32 guard = fConsoleSize;
	while (fConsoleRead != writeIndex && guard-- > 0) {
		char c = (char)_TcmRead1(fConsoleBuffer + fConsoleRead);
		if (++fConsoleRead == fConsoleSize)
			fConsoleRead = 0;
		if (c == '\r')
			continue;
		if (c == '\n' || length == sizeof(line) - 1) {
			line[length] = '\0';
			if (length > 0)
				INFO("firmware: %s\n", line);
			length = 0;
			if (c == '\n')
				continue;
		}
		if (c >= ' ' && c < 0x7f)
			line[length++] = c;
	}
	if (length > 0) {
		line[length] = '\0';
		INFO("firmware: %s\n", line);
	}
	atomic_set(&fConsoleBusy, 0);
}


status_t
BrcmfPcie::Start()
{
	if (fRunning)
		return B_OK;

	status_t status = fChip.Attach(this);
	if (status != B_OK) {
		ERROR("chip attach failed: %s\n", strerror(status));
		return status;
	}

	// as bwfm_pci_preinit(): read and write back the BAR2 config
	_SelectCore(BRCMF_CORE_PCIE2);
	_PcieWrite(PCIE2_CONFIGADDR, 0x4e0);
	_PcieWrite(PCIE2_CONFIGDATA, _PcieRead(PCIE2_CONFIGDATA));

	const char* chip = _FirmwareName();
	if (chip == NULL) {
		ERROR("no firmware known for chip BCM%s revision %u\n", fChip.Name(),
			fChip.ChipRev());
		return B_NOT_SUPPORTED;
	}

	char name[64];
	snprintf(name, sizeof(name), "brcmfmac%s-pcie.bin", chip);
	uint8* firmware;
	size_t size;
	status = read_firmware_file(name, &firmware, &size);
	if (status != B_OK) {
		ERROR("cannot read firmware %s from firmware/brcm: %s\n", name,
			strerror(status));
		return status;
	}

	if (size >= kFirmwareRamSizeOffset + 8
		&& get_le32(firmware + kFirmwareRamSizeOffset)
			== kFirmwareRamSizeMagic) {
		uint32 ramSize = get_le32(firmware + kFirmwareRamSizeOffset + 4);
		INFO("firmware asks for %" B_PRIu32 " KB of RAM\n", ramSize / 1024);
		fChip.SetRamSize(ramSize);
	}

	status = _LoadFirmware(firmware, size);
	free(firmware);
	if (status != B_OK) {
		fChip.SetPassive();
		return status;
	}
	fRunning = true;

	_ReadShared();
	// let the firmware finish its start-up messages
	snooze(200000);
	_DumpConsole();

	status = _SetupMsgbuf();
	if (status != B_OK) {
		ERROR("msgbuf set-up failed: %s\n", strerror(status));
		Stop();
		return status;
	}
	_QueryFirmware();
	return B_OK;
}


void
BrcmfPcie::Stop()
{
	if (!fRunning)
		return;

	// Stop the firmware before freeing the memory it writes into.
	_DumpConsole();
	if (fMsgbufReady)
		_EnableInterrupts(false);
	fChip.SetPassive();
	fRunning = false;
	_FreeInterrupts();
	_FreeMsgbuf();
	INFO("firmware stopped\n");
}
