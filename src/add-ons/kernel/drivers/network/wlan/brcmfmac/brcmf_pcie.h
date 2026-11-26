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
 * PCIe bus side of the brcmfmac driver, for the Broadcom FullMAC chips on
 * PCIe (BCM4350 in the MacBookPro14,1 and others). Registers and bring-up
 * order follow OpenBSD's bwfm(4) PCIe attachment (sys/dev/pci/if_bwfm_pci.c,
 * if_bwfm_pci.h), hence its notices above.
 *
 * Phase 1 (brcmf_pcie.cpp): the card is mapped, the firmware is loaded and
 * started, and the shared memory area the firmware sets up is read.
 * Phase 2 (brcmf_pcie_msgbuf.cpp): the msgbuf rings in host memory, the
 * interrupt, and firmware commands (dcmds/iovars) over the control rings.
 * The data path (flow rings, RX buffers) comes next.
 */
#ifndef BRCMF_PCIE_H
#define BRCMF_PCIE_H


#include <PCI.h>
#include <bus/PCI.h>

#include <condition_variable.h>
#include <lock.h>

#include "brcmf_chip.h"


struct brcmf_dma_buffer {
	area_id		area;
	uint8*		data;
	phys_addr_t	physical;
	size_t		size;
};


struct brcmf_msgring {
	const char*	name;
	brcmf_dma_buffer memory;
	uint32		writeIndexAddress;	// in the chip's RAM
	uint32		readIndexAddress;
	uint16		writePointer;
	uint16		readPointer;
	uint16		items;
	uint16		itemSize;
};


// Buffers posted to the firmware for its control replies and events.
#define BRCMF_PCIE_IOCTL_BUFFERS	8
#define BRCMF_PCIE_EVENT_BUFFERS	8
#define BRCMF_PCIE_CONTROL_BUFFERS \
	(BRCMF_PCIE_IOCTL_BUFFERS + BRCMF_PCIE_EVENT_BUFFERS)


class BrcmfPcie : public BrcmfBackplane {
public:
								BrcmfPcie(pci_device_module_info* pci,
									pci_device* device);
	virtual						~BrcmfPcie();

			status_t			Init();
			status_t			Start();
			void				Stop();

			// firmware commands; *_length is in: buffer size, out: reply
			status_t			Dcmd(uint32 command, bool set, void* data,
									size_t* _length);
			status_t			GetVar(const char* name, void* data,
									size_t length);

	// BrcmfBackplane
	virtual	uint32				Read4(uint32 address);
	virtual	void				Write4(uint32 address, uint32 value);
	virtual	void				ResetBus();
	virtual	void				WriteResetVector(uint32 vector);

private:
			status_t			_MapBar(int index, const char* name,
									area_id* _area, volatile uint8** _base,
									size_t* _size, int* _nextIndex);
			void				_SelectCore(uint16 id);
			void				_SetWindow(uint32 address);

			uint32				_RegRead(uint32 offset);
			void				_RegWrite(uint32 offset, uint32 value);
			uint32				_PcieRead(uint32 offset);
			void				_PcieWrite(uint32 offset, uint32 value);
			uint32				_TcmRead4(uint32 address);
			void				_TcmWrite4(uint32 address, uint32 value);
			uint8				_TcmRead1(uint32 address);

			uint32				_ConfigRead(uint16 offset);
			void				_ConfigWrite(uint16 offset, uint32 value);

			uint32				_InterruptStatus();
			void				_AckInterrupts(uint32 status);

			const char*			_FirmwareName();
			status_t			_LoadFirmware(const uint8* firmware,
									size_t size);
			void				_ReadShared();
			void				_DumpConsole();

			// msgbuf (brcmf_pcie_msgbuf.cpp)
			uint16				_TcmRead2(uint32 address);
			void				_TcmWrite2(uint32 address, uint16 value);
			status_t			_AllocDma(brcmf_dma_buffer& buffer,
									size_t size, const char* name);
			void				_FreeDma(brcmf_dma_buffer& buffer);

			status_t			_SetupMsgbuf();
			void				_FreeMsgbuf();
			status_t			_SetupRing(brcmf_msgring& ring,
									const char* name, uint16 items,
									uint16 itemSize, uint32 writeIndexBase,
									uint32 readIndexBase, int index,
									uint32 indexStride, uint32& ringMemory);
			void*				_RingReserve(brcmf_msgring& ring);
			void				_RingCommit(brcmf_msgring& ring);
			int32				_RingProcess(brcmf_msgring& ring);
			void				_HandleMessage(const uint8* message);
			void				_PostControlBuffers();

			status_t			_SetupInterrupts();
			void				_FreeInterrupts();
			void				_EnableInterrupts(bool enable);
			static int32		_InterruptHandler(void* data);
			static status_t		_ServiceThread(void* data);
			void				_ServiceLoop();
			void				_HandleMailboxData();
			void				_QueryFirmware();

private:
			pci_device_module_info* fPci;
			pci_device*			fDevice;
			pci_info			fInfo;

			area_id				fRegArea;
			volatile uint8*		fRegs;
			size_t				fRegSize;
			area_id				fTcmArea;
			volatile uint8*		fTcm;
			size_t				fTcmSize;
			uint32				fWindow;

			BrcmfChip			fChip;
			bool				fRegisters64;
			bool				fRunning;

			uint32				fSharedAddress;
			uint32				fSharedFlags;
			uint32				fSharedVersion;
			uint32				fConsoleAddress;
			uint32				fConsoleBuffer;
			uint32				fConsoleSize;
			uint32				fConsoleRead;
			int32				fConsoleBusy;

			// msgbuf
			bool				fMsgbufReady;
			uint32				fRxDataOffset;
			uint32				fMaxRxBufPost;
			uint32				fHostToDeviceMailbox;
			uint32				fDeviceToHostMailbox;
			uint32				fMaxSubmissionRings;
			uint32				fMaxFlowRings;
			uint32				fMaxCompletionRings;
			brcmf_msgring		fControlSubmit;
			brcmf_msgring		fRxPostSubmit;
			brcmf_msgring		fControlComplete;
			brcmf_msgring		fTxComplete;
			brcmf_msgring		fRxComplete;
			brcmf_dma_buffer	fScratch;
			brcmf_dma_buffer	fRingUpdate;
			brcmf_dma_buffer	fIoctlRequest;
			brcmf_dma_buffer	fControlBuffers[BRCMF_PCIE_CONTROL_BUFFERS];
			bool				fControlPosted[BRCMF_PCIE_CONTROL_BUFFERS];
			mutex				fRingLock;

			// commands
			mutex				fIoctlLock;
			ConditionVariable	fIoctlCondition;
			uint16				fIoctlTransaction;
			bool				fIoctlDone;
			int16				fIoctlStatus;
			uint8*				fIoctlReply;
			size_t				fIoctlReplyLength;

			// interrupt
			uint32				fIrq;
			bool				fMsi;
			bool				fHandlerInstalled;
			sem_id				fServiceSem;
			thread_id			fServiceThreadId;
			volatile bool		fStopService;
			int32				fPendingStatus;
			int32				fInterruptCount;
			int32				fPolledMessages;
			int32				fEventCount;
};


#endif	// BRCMF_PCIE_H
