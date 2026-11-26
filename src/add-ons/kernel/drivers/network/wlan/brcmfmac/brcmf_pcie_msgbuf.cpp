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
 * The msgbuf protocol of the Broadcom FullMAC PCIe chips: message rings in
 * host memory, their read and write indices in the chip's RAM, doorbells,
 * the interrupt, and firmware commands (dcmds) over the control rings.
 *
 * Provenance: the ring layout and handling, the message formats, the ring
 * sizes and the command exchange follow OpenBSD's bwfm(4) PCIe attachment
 * (sys/dev/pci/if_bwfm_pci.c 1.79 and if_bwfm_pci.h 1.9:
 * bwfm_pci_preinit() from the ring info on, bwfm_pci_setup_ring(), the
 * bwfm_pci_ring_*() helpers, bwfm_pci_msg_rx(), bwfm_pci_intr*(),
 * bwfm_pci_fill_rx_ioctl_ring(), bwfm_pci_msgbuf_query_dcmd() and
 * bwfm_pci_msgbuf_rxioctl()), Patrick Wildt and Broadcom Corporation, ISC.
 * Linux brcmfmac (msgbuf.c, pcie.c; ISC) compared only.
 *
 * Differences: firmware with DMA'd ring indices (shared flag DMA_INDEX) is
 * not handled yet, as the BCM4350 firmware keeps them in its RAM; the host
 * memory is below 4 GB; a service thread does the work of the interrupt,
 * and also polls, so that a missing interrupt shows in the syslog instead
 * of hanging the driver.
 */


#include "brcmf_pcie.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <KernelExport.h>
#include <vm/vm.h>

#include "brcmfmac.h"
#include "brcmf_pcie_regs.h"


// Ring geometry, as bwfm_pci_preinit()
static const uint16 kControlSubmitItems = 64;
static const uint16 kControlSubmitItemSize = 40;
static const uint16 kRxPostItems = 1024;
static const uint16 kRxPostItemSize = 32;
static const uint16 kControlCompleteItems = 64;
static const uint16 kControlCompleteItemSize = 24;
static const uint16 kTxCompleteItems = 1024;
static const uint16 kRxCompleteItems = 1024;

static const uint32 kSubmissionCommonRings = 2;	// control, RX post
static const uint32 kCompletionRings = 3;		// control, TX, RX

static const size_t kScratchSize = 8;
static const size_t kRingUpdateSize = 1024;
static const size_t kIoctlRequestSize = 1518;	// ETHER_MAX_LEN, as bwfm
static const size_t kControlBufferSize = 8192;	// MSGBUF_MAX_CTL_PKT_SIZE
static const uint32 kIoctlRequestId = 0xfffe;	// MSGBUF_IOCTL_REQ_PKTID
static const bigtime_t kIoctlTimeout = 2000000;
static const bigtime_t kPollInterval = 100000;

// dcmds
static const uint32 kDcmdGetVar = 262;

// message types
enum {
	MSGBUF_TYPE_GEN_STATUS				= 0x01,
	MSGBUF_TYPE_RING_STATUS				= 0x02,
	MSGBUF_TYPE_FLOW_RING_CREATE_CMPLT	= 0x04,
	MSGBUF_TYPE_FLOW_RING_DELETE_CMPLT	= 0x06,
	MSGBUF_TYPE_IOCTLPTR_REQ			= 0x09,
	MSGBUF_TYPE_IOCTLPTR_REQ_ACK		= 0x0a,
	MSGBUF_TYPE_IOCTLRESP_BUF_POST		= 0x0b,
	MSGBUF_TYPE_IOCTL_CMPLT				= 0x0c,
	MSGBUF_TYPE_EVENT_BUF_POST			= 0x0d,
	MSGBUF_TYPE_WL_EVENT				= 0x0e,
	MSGBUF_TYPE_TX_STATUS				= 0x10,
	MSGBUF_TYPE_RX_CMPLT				= 0x12,
	MSGBUF_TYPE_D2H_MAILBOX_DATA		= 0x24,
};

// Device to host mailbox bits
static const uint32 kDeviceD3Ack = 0x00000001;
static const uint32 kDeviceDeepSleepEnter = 0x00000002;
static const uint32 kDeviceDeepSleepExit = 0x00000004;
static const uint32 kDeviceFirmwareHalt = 0x10000000;


// Message formats (little endian, as the host)

struct msgbuf_common_header {
	uint8	type;
	uint8	interface;
	uint8	flags;
	uint8	reserved;
	uint32	request_id;
} _PACKED;

struct msgbuf_address {
	uint32	low;
	uint32	high;
} _PACKED;

struct msgbuf_ioctl_request {
	msgbuf_common_header header;
	uint32	command;
	uint16	transaction;
	uint16	input_length;
	uint16	output_length;
	uint16	reserved0[3];
	msgbuf_address request_buffer;
	uint32	reserved1[2];
} _PACKED;

struct msgbuf_buffer_post {
	msgbuf_common_header header;
	uint16	buffer_length;
	uint16	reserved0[3];
	msgbuf_address buffer;
	uint32	reserved1[4];
} _PACKED;

struct msgbuf_completion_header {
	uint16	status;
	uint16	flow_ring;
} _PACKED;

struct msgbuf_ioctl_response {
	msgbuf_common_header header;
	msgbuf_completion_header completion;
	uint16	length;
	uint16	transaction;
	uint32	command;
	uint32	reserved;
} _PACKED;

struct msgbuf_event {
	msgbuf_common_header header;
	msgbuf_completion_header completion;
	uint16	length;
	uint16	sequence;
	uint16	reserved[4];
} _PACKED;

struct msgbuf_status {
	msgbuf_common_header header;
	msgbuf_completion_header completion;
	uint32	data;
} _PACKED;

static_assert(sizeof(msgbuf_ioctl_request) == 40, "request size");
static_assert(sizeof(msgbuf_buffer_post) == 40, "buffer post size");
static_assert(sizeof(msgbuf_ioctl_response) <= 24, "response size");


// #pragma mark - helpers


uint16
BrcmfPcie::_TcmRead2(uint32 address)
{
	if (address + 2 > fTcmSize)
		return 0xffff;
	return *(volatile uint16*)(fTcm + address);
}


void
BrcmfPcie::_TcmWrite2(uint32 address, uint16 value)
{
	if (address + 2 > fTcmSize)
		return;
	*(volatile uint16*)(fTcm + address) = value;
}


/*!	Physically contiguous, zeroed memory below 4 GB that the chip reads and
	writes. PCI DMA is cache coherent on x86, so no syncing is needed.
*/
status_t
BrcmfPcie::_AllocDma(brcmf_dma_buffer& buffer, size_t size, const char* name)
{
	size_t areaSize = (size + B_PAGE_SIZE - 1) & ~(size_t)(B_PAGE_SIZE - 1);

	virtual_address_restrictions virtualRestrictions = {};
	virtualRestrictions.address_specification = B_ANY_KERNEL_ADDRESS;
	physical_address_restrictions physicalRestrictions = {};
	physicalRestrictions.high_address = 0x100000000ULL;

	void* address;
	area_id area = create_area_etc(B_SYSTEM_TEAM, name, areaSize,
		B_CONTIGUOUS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, 0, 0,
		&virtualRestrictions, &physicalRestrictions, &address);
	if (area < 0) {
		ERROR("cannot allocate %s (%" B_PRIuSIZE " bytes): %s\n", name,
			areaSize, strerror(area));
		return area;
	}

	physical_entry entry;
	status_t status = get_memory_map(address, areaSize, &entry, 1);
	if (status != B_OK) {
		delete_area(area);
		return status;
	}

	memset(address, 0, areaSize);
	buffer.area = area;
	buffer.data = (uint8*)address;
	buffer.physical = entry.address;
	buffer.size = size;
	return B_OK;
}


void
BrcmfPcie::_FreeDma(brcmf_dma_buffer& buffer)
{
	if (buffer.area >= 0)
		delete_area(buffer.area);
	buffer.area = -1;
	buffer.data = NULL;
	buffer.physical = 0;
	buffer.size = 0;
}


// #pragma mark - rings


status_t
BrcmfPcie::_SetupRing(brcmf_msgring& ring, const char* name, uint16 items,
	uint16 itemSize, uint32 writeIndexBase, uint32 readIndexBase, int index,
	uint32 indexStride, uint32& ringMemory)
{
	ring.name = name;
	ring.writeIndexAddress = writeIndexBase + index * indexStride;
	ring.readIndexAddress = readIndexBase + index * indexStride;
	ring.writePointer = 0;
	ring.readPointer = 0;
	ring.items = items;
	ring.itemSize = itemSize;
	_TcmWrite2(ring.readIndexAddress, 0);
	_TcmWrite2(ring.writeIndexAddress, 0);

	status_t status = _AllocDma(ring.memory, (size_t)items * itemSize, name);
	if (status != B_OK)
		return status;

	_TcmWrite4(ringMemory + RING_BASE_ADDR_LOW,
		(uint32)(ring.memory.physical & 0xffffffff));
	_TcmWrite4(ringMemory + RING_BASE_ADDR_HIGH,
		(uint32)((uint64)ring.memory.physical >> 32));
	_TcmWrite2(ringMemory + RING_MAX_ITEM, items);
	_TcmWrite2(ringMemory + RING_LEN_ITEMS, itemSize);
	ringMemory += RING_DESCRIPTOR_SIZE;
	return B_OK;
}


/*!	Returns the next free item of a submission ring, or NULL if it is full.
	Call with the ring lock held; nothing is sent before _RingCommit().
*/
void*
BrcmfPcie::_RingReserve(brcmf_msgring& ring)
{
	ring.readPointer = _TcmRead2(ring.readIndexAddress);
	if (ring.readPointer >= ring.items)
		return NULL;

	int available;
	if (ring.readPointer > ring.writePointer)
		available = ring.readPointer - ring.writePointer;
	else
		available = ring.readPointer + (ring.items - ring.writePointer);
	if (available <= 1)
		return NULL;

	void* item = ring.memory.data + ring.writePointer * ring.itemSize;
	memset(item, 0, ring.itemSize);
	if (++ring.writePointer == ring.items)
		ring.writePointer = 0;
	return item;
}


void
BrcmfPcie::_RingCommit(brcmf_msgring& ring)
{
	memory_write_barrier();
	_TcmWrite2(ring.writeIndexAddress, ring.writePointer);
	_PcieWrite(PCIE2_H2D_MAILBOX_0, 1);
}


/*!	Hands every new item of a completion ring to _HandleMessage(), and tells
	the firmware they were read. Returns the number of items.
*/
int32
BrcmfPcie::_RingProcess(brcmf_msgring& ring)
{
	int32 total = 0;
	while (true) {
		ring.writePointer = _TcmRead2(ring.writeIndexAddress);
		if (ring.writePointer >= ring.items) {
			ERROR("%s ring: bad write index %u\n", ring.name,
				ring.writePointer);
			return total;
		}

		int available;
		if (ring.writePointer >= ring.readPointer)
			available = ring.writePointer - ring.readPointer;
		else
			available = ring.items - ring.readPointer;
		if (available == 0)
			return total;

		memory_read_barrier();
		uint8* item = ring.memory.data + ring.readPointer * ring.itemSize;
		for (int i = 0; i < available; i++) {
			_HandleMessage(item + fRxDataOffset);
			item += ring.itemSize;
		}

		ring.readPointer += available;
		if (ring.readPointer == ring.items)
			ring.readPointer = 0;
		_TcmWrite2(ring.readIndexAddress, ring.readPointer);
		total += available;
	}
}


/*!	Gives the firmware every control buffer it does not hold: the first
	BRCMF_PCIE_IOCTL_BUFFERS for command replies, the rest for events. The
	request id is the buffer's index.
*/
void
BrcmfPcie::_PostControlBuffers()
{
	MutexLocker locker(fRingLock);

	bool posted = false;
	for (int i = 0; i < BRCMF_PCIE_CONTROL_BUFFERS; i++) {
		if (fControlPosted[i] || fControlBuffers[i].area < 0)
			continue;

		msgbuf_buffer_post* post
			= (msgbuf_buffer_post*)_RingReserve(fControlSubmit);
		if (post == NULL)
			break;

		post->header.type = i < BRCMF_PCIE_IOCTL_BUFFERS
			? MSGBUF_TYPE_IOCTLRESP_BUF_POST : MSGBUF_TYPE_EVENT_BUF_POST;
		post->header.request_id = i;
		post->buffer_length = kControlBufferSize;
		post->buffer.low = (uint32)(fControlBuffers[i].physical & 0xffffffff);
		post->buffer.high = (uint32)((uint64)fControlBuffers[i].physical
			>> 32);
		fControlPosted[i] = true;
		posted = true;
	}
	if (posted)
		_RingCommit(fControlSubmit);
}


// #pragma mark - messages


void
BrcmfPcie::_HandleMessage(const uint8* message)
{
	const msgbuf_common_header* header
		= (const msgbuf_common_header*)message;

	switch (header->type) {
		case MSGBUF_TYPE_IOCTLPTR_REQ_ACK:
			// the request buffer may be reused; it is, one at a time
			break;

		case MSGBUF_TYPE_IOCTL_CMPLT:
		{
			const msgbuf_ioctl_response* response
				= (const msgbuf_ioctl_response*)message;
			uint32 index = response->header.request_id;
			if (index >= BRCMF_PCIE_IOCTL_BUFFERS || !fControlPosted[index]) {
				ERROR("command reply in unknown buffer %" B_PRIu32 "\n",
					index);
				break;
			}

			MutexLocker locker(fRingLock);
			fControlPosted[index] = false;
			if (response->transaction == fIoctlTransaction && !fIoctlDone) {
				size_t length = min_c((size_t)response->length,
					kControlBufferSize);
				memcpy(fIoctlReply, fControlBuffers[index].data, length);
				fIoctlReplyLength = length;
				fIoctlStatus = (int16)response->completion.status;
				fIoctlDone = true;
				fIoctlCondition.NotifyAll();
			} else {
				ERROR("late reply to command transaction %u\n",
					response->transaction);
			}
			break;
		}

		case MSGBUF_TYPE_WL_EVENT:
		{
			const msgbuf_event* event = (const msgbuf_event*)message;
			uint32 index = event->header.request_id;
			if (index < BRCMF_PCIE_IOCTL_BUFFERS
				|| index >= BRCMF_PCIE_CONTROL_BUFFERS
				|| !fControlPosted[index]) {
				ERROR("event in unknown buffer %" B_PRIu32 "\n", index);
				break;
			}
			fEventCount++;
			size_t length = min_c((size_t)event->length,
				kControlBufferSize - fRxDataOffset);
			if (fReceiver != NULL) {
				fReceiver->ReceiveEvent(fControlBuffers[index].data
					+ fRxDataOffset, length);
			}
			MutexLocker locker(fRingLock);
			fControlPosted[index] = false;
			break;
		}

		case MSGBUF_TYPE_RX_CMPLT:
			_HandleRxComplete(message);
			break;

		case MSGBUF_TYPE_TX_STATUS:
			_HandleTxStatus(message);
			break;

		case MSGBUF_TYPE_FLOW_RING_CREATE_CMPLT:
			_HandleFlowRingCreated(message);
			break;

		case MSGBUF_TYPE_D2H_MAILBOX_DATA:
		{
			const msgbuf_status* status = (const msgbuf_status*)message;
			INFO("mailbox data %#" B_PRIx32 " from the firmware\n",
				status->data);
			break;
		}

		case MSGBUF_TYPE_GEN_STATUS:
		case MSGBUF_TYPE_RING_STATUS:
		{
			const msgbuf_status* status = (const msgbuf_status*)message;
			INFO("%s status %u (ring %u)\n",
				header->type == MSGBUF_TYPE_GEN_STATUS ? "general" : "ring",
				status->completion.status, status->completion.flow_ring);
			break;
		}

		default:
			INFO("unhandled message type %#x\n", header->type);
			break;
	}
}


void
BrcmfPcie::_HandleMailboxData()
{
	if (fDeviceToHostMailbox == 0)
		return;
	uint32 data = _TcmRead4(fDeviceToHostMailbox);
	if (data == 0)
		return;
	_TcmWrite4(fDeviceToHostMailbox, 0);

	INFO("mailbox from the firmware: %#" B_PRIx32 "%s%s%s%s\n", data,
		(data & kDeviceD3Ack) != 0 ? ", D3 ack" : "",
		(data & kDeviceDeepSleepEnter) != 0 ? ", deep sleep enter" : "",
		(data & kDeviceDeepSleepExit) != 0 ? ", deep sleep exit" : "",
		(data & kDeviceFirmwareHalt) != 0 ? ", FIRMWARE HALTED" : "");
	if ((data & kDeviceFirmwareHalt) != 0)
		_DumpConsole();
}


// #pragma mark - interrupt


void
BrcmfPcie::_EnableInterrupts(bool enable)
{
	if (fRegisters64) {
		_PcieWrite(PCIE2_64_MAILBOXMASK, enable ? MAILBOX_64_INT_D2H_DB : 0);
	} else {
		_PcieWrite(PCIE2_MAILBOXMASK, enable
			? MAILBOX_INT_FN0_0 | MAILBOX_INT_FN0_1 | MAILBOX_INT_D2H_DB : 0);
	}
}


int32
BrcmfPcie::_InterruptHandler(void* data)
{
	BrcmfPcie* self = (BrcmfPcie*)data;

	uint32 status = self->_InterruptStatus();
	if (status == 0 || status == 0xffffffff)
		return B_UNHANDLED_INTERRUPT;

	self->_EnableInterrupts(false);
	self->_AckInterrupts(status);
	atomic_or(&self->fPendingStatus, (int32)status);
	atomic_add(&self->fInterruptCount, 1);
	release_sem_etc(self->fServiceSem, 1, B_DO_NOT_RESCHEDULE);
	return B_INVOKE_SCHEDULER;
}


status_t
BrcmfPcie::_ServiceThread(void* data)
{
	((BrcmfPcie*)data)->_ServiceLoop();
	return B_OK;
}


void
BrcmfPcie::_ServiceLoop()
{
	while (!fStopService) {
		status_t status = acquire_sem_etc(fServiceSem, 1, B_RELATIVE_TIMEOUT,
			kPollInterval);
		if (fStopService)
			break;
		bool interrupted = status == B_OK;

		uint32 pending = (uint32)atomic_and(&fPendingStatus, 0);
		if ((pending & (MAILBOX_INT_FN0_0 | MAILBOX_INT_FN0_1)) != 0
			&& !fRegisters64) {
			_HandleMailboxData();
		}

		int32 handled = _RingProcess(fControlComplete);
		if (handled > 0)
			_PostControlBuffers();
		handled += _RingProcess(fTxComplete);
		int32 received = _RingProcess(fRxComplete);
		if (received > 0)
			_PostRxBuffers();
		handled += received;
		if (handled > 0 && !interrupted)
			atomic_add(&fPolledMessages, handled);

		if (interrupted)
			_EnableInterrupts(true);
		else
			_DumpConsole();
	}
}


status_t
BrcmfPcie::_SetupInterrupts()
{
	fServiceSem = create_sem(0, "brcmfmac pcie service");
	if (fServiceSem < 0)
		return fServiceSem;

	if (fPci->get_msi_count(fDevice) >= 1) {
		uint32 vector;
		if (fPci->configure_msi(fDevice, 1, &vector) == B_OK) {
			if (fPci->enable_msi(fDevice) == B_OK) {
				fIrq = vector;
				fMsi = true;
			} else
				fPci->unconfigure_msi(fDevice);
		}
	}
	if (!fMsi)
		fIrq = fInfo.u.h0.interrupt_line;

	if (fIrq != 0 && fIrq != 0xff) {
		status_t status = install_io_interrupt_handler(fIrq,
			&_InterruptHandler, this, 0);
		if (status == B_OK)
			fHandlerInstalled = true;
		else {
			ERROR("cannot install the interrupt handler on %" B_PRIu32
				": %s; polling only\n", fIrq, strerror(status));
		}
	} else
		ERROR("no interrupt assigned; polling only\n");
	INFO("interrupt %" B_PRIu32 " (%s)%s\n", fIrq, fMsi ? "MSI" : "INTx",
		fHandlerInstalled ? "" : ", not used");

	fStopService = false;
	fServiceThreadId = spawn_kernel_thread(&_ServiceThread,
		"brcmfmac pcie service", B_REAL_TIME_DISPLAY_PRIORITY, this);
	if (fServiceThreadId < 0)
		return fServiceThreadId;
	resume_thread(fServiceThreadId);
	return B_OK;
}


void
BrcmfPcie::_FreeInterrupts()
{
	if (fHandlerInstalled) {
		remove_io_interrupt_handler(fIrq, &_InterruptHandler, this);
		fHandlerInstalled = false;
	}
	if (fMsi) {
		fPci->disable_msi(fDevice);
		fPci->unconfigure_msi(fDevice);
		fMsi = false;
	}
	if (fServiceThreadId >= 0) {
		fStopService = true;
		release_sem(fServiceSem);
		status_t result;
		wait_for_thread(fServiceThreadId, &result);
		fServiceThreadId = -1;
	}
	if (fServiceSem >= 0) {
		delete_sem(fServiceSem);
		fServiceSem = -1;
	}
}


// #pragma mark - set-up


/*!	As bwfm_pci_preinit() after the firmware is up: reads the ring info,
	puts the five common rings into host memory, gives the firmware its
	scratch buffers, switches the interrupt on, and posts the buffers for
	command replies and events.
*/
status_t
BrcmfPcie::_SetupMsgbuf()
{
	if ((fSharedFlags & SHARED_INFO_DMA_INDEX) != 0) {
		ERROR("firmware wants DMA'd ring indices; not supported yet\n");
		return B_NOT_SUPPORTED;
	}

	fMaxRxBufPost = _TcmRead2(fSharedAddress + SHARED_MAX_RXBUFPOST);
	if (fMaxRxBufPost == 0)
		fMaxRxBufPost = 255;
	fRxDataOffset = _TcmRead4(fSharedAddress + SHARED_RX_DATAOFFSET);
	if (fRxDataOffset >= kControlCompleteItemSize) {
		ERROR("RX data offset %" B_PRIu32 " too large\n", fRxDataOffset);
		return B_BAD_DATA;
	}
	fHostToDeviceMailbox = _TcmRead4(fSharedAddress
		+ SHARED_HTOD_MB_DATA_ADDR);
	fDeviceToHostMailbox = _TcmRead4(fSharedAddress
		+ SHARED_DTOH_MB_DATA_ADDR);

	uint32 ramBase = fChip.RamBase();
	uint32 ramEnd = ramBase + fChip.RamSize();
	uint32 ringInfo = _TcmRead4(fSharedAddress + SHARED_RING_INFO_ADDR);
	if (ringInfo < ramBase || ringInfo + 0x40 > ramEnd) {
		ERROR("ring info at %#" B_PRIx32 " is outside the RAM\n", ringInfo);
		return B_BAD_DATA;
	}

	uint32 ringMemory = _TcmRead4(ringInfo + RINGINFO_RINGMEM);
	uint32 h2dWrite = _TcmRead4(ringInfo + RINGINFO_H2D_W_IDX_PTR);
	uint32 h2dRead = _TcmRead4(ringInfo + RINGINFO_H2D_R_IDX_PTR);
	uint32 d2hWrite = _TcmRead4(ringInfo + RINGINFO_D2H_W_IDX_PTR);
	uint32 d2hRead = _TcmRead4(ringInfo + RINGINFO_D2H_R_IDX_PTR);
	fH2DWriteBase = h2dWrite;
	fH2DReadBase = h2dRead;
	if (fSharedVersion >= 6) {
		fMaxSubmissionRings = _TcmRead2(ringInfo + RINGINFO_MAX_SUBMISSION);
		fMaxFlowRings = _TcmRead2(ringInfo + RINGINFO_MAX_FLOWRINGS);
		fMaxCompletionRings = _TcmRead2(ringInfo + RINGINFO_MAX_COMPLETION);
	} else {
		fMaxSubmissionRings = _TcmRead2(ringInfo + RINGINFO_MAX_FLOWRINGS);
		fMaxFlowRings = fMaxSubmissionRings - kSubmissionCommonRings;
		fMaxCompletionRings = kCompletionRings;
	}
	INFO("rings: descriptors at %#" B_PRIx32 ", indices H2D %#" B_PRIx32
		"/%#" B_PRIx32 ", D2H %#" B_PRIx32 "/%#" B_PRIx32 ", %" B_PRIu32
		" submission, %" B_PRIu32 " flow, %" B_PRIu32 " completion rings\n",
		ringMemory, h2dWrite, h2dRead, d2hWrite, d2hRead,
		fMaxSubmissionRings, fMaxFlowRings, fMaxCompletionRings);

	uint32 addresses[] = { ringMemory, h2dWrite, h2dRead, d2hWrite, d2hRead };
	for (size_t i = 0; i < B_COUNT_OF(addresses); i++) {
		if (addresses[i] < ramBase || addresses[i] >= ramEnd) {
			ERROR("ring info entry %" B_PRIuSIZE " (%#" B_PRIx32
				") is outside the RAM\n", i, addresses[i]);
			return B_BAD_DATA;
		}
	}
	if (fMaxSubmissionRings < kSubmissionCommonRings
		|| fMaxSubmissionRings > 1024) {
		ERROR("unexpected number of submission rings\n");
		return B_BAD_DATA;
	}

	const uint32 stride = sizeof(uint32);
	status_t status = _SetupRing(fControlSubmit, "brcmfmac control submit",
		kControlSubmitItems, kControlSubmitItemSize, h2dWrite, h2dRead, 0,
		stride, ringMemory);
	if (status == B_OK) {
		status = _SetupRing(fRxPostSubmit, "brcmfmac rx post", kRxPostItems,
			kRxPostItemSize, h2dWrite, h2dRead, 1, stride, ringMemory);
	}
	if (status == B_OK) {
		status = _SetupRing(fControlComplete, "brcmfmac control complete",
			kControlCompleteItems, kControlCompleteItemSize, d2hWrite,
			d2hRead, 0, stride, ringMemory);
	}
	if (status == B_OK) {
		status = _SetupRing(fTxComplete, "brcmfmac tx complete",
			kTxCompleteItems, fSharedVersion >= 7 ? 24 : 16, d2hWrite,
			d2hRead, 1, stride, ringMemory);
	}
	if (status == B_OK) {
		status = _SetupRing(fRxComplete, "brcmfmac rx complete",
			kRxCompleteItems, fSharedVersion >= 7 ? 40 : 32, d2hWrite,
			d2hRead, 2, stride, ringMemory);
	}
	if (status != B_OK)
		return status;

	status = _AllocDma(fScratch, kScratchSize, "brcmfmac scratch");
	if (status == B_OK) {
		status = _AllocDma(fRingUpdate, kRingUpdateSize,
			"brcmfmac ring update");
	}
	if (status == B_OK) {
		status = _AllocDma(fIoctlRequest, kIoctlRequestSize,
			"brcmfmac ioctl request");
	}
	for (int i = 0; i < BRCMF_PCIE_CONTROL_BUFFERS && status == B_OK; i++) {
		status = _AllocDma(fControlBuffers[i], kControlBufferSize,
			"brcmfmac control buffer");
	}
	if (status == B_OK) {
		fIoctlReply = (uint8*)malloc(kControlBufferSize);
		if (fIoctlReply == NULL)
			status = B_NO_MEMORY;
	}
	if (status != B_OK)
		return status;

	_TcmWrite4(fSharedAddress + SHARED_DMA_SCRATCH_ADDR_LOW,
		(uint32)(fScratch.physical & 0xffffffff));
	_TcmWrite4(fSharedAddress + SHARED_DMA_SCRATCH_ADDR_HIGH,
		(uint32)((uint64)fScratch.physical >> 32));
	_TcmWrite4(fSharedAddress + SHARED_DMA_SCRATCH_LEN, kScratchSize);
	_TcmWrite4(fSharedAddress + SHARED_DMA_RINGUPD_ADDR_LOW,
		(uint32)(fRingUpdate.physical & 0xffffffff));
	_TcmWrite4(fSharedAddress + SHARED_DMA_RINGUPD_ADDR_HIGH,
		(uint32)((uint64)fRingUpdate.physical >> 32));
	_TcmWrite4(fSharedAddress + SHARED_DMA_RINGUPD_LEN, kRingUpdateSize);

	status = _SetupInterrupts();
	if (status != B_OK)
		return status;

	_SelectCore(BRCMF_CORE_PCIE2);
	fMsgbufReady = true;
	_EnableInterrupts(true);
	if ((fSharedFlags & SHARED_INFO_HOSTRDY_DB1) != 0)
		_PcieWrite(PCIE2_H2D_MAILBOX_1, 1);

	_PostControlBuffers();
	INFO("msgbuf rings set up\n");

	status = _SetupData();
	if (status != B_OK)
		return status;
	return B_OK;
}


void
BrcmfPcie::_FreeMsgbuf()
{
	fMsgbufReady = false;
	_FreeData();
	_FreeDma(fControlSubmit.memory);
	_FreeDma(fRxPostSubmit.memory);
	_FreeDma(fControlComplete.memory);
	_FreeDma(fTxComplete.memory);
	_FreeDma(fRxComplete.memory);
	_FreeDma(fScratch);
	_FreeDma(fRingUpdate);
	_FreeDma(fIoctlRequest);
	for (int i = 0; i < BRCMF_PCIE_CONTROL_BUFFERS; i++) {
		_FreeDma(fControlBuffers[i]);
		fControlPosted[i] = false;
	}
	free(fIoctlReply);
	fIoctlReply = NULL;
}


// #pragma mark - commands


/*!	Sends a dcmd (firmware command) and waits for its reply, as
	bwfm_pci_msgbuf_query_dcmd(). \a data holds the request and gets the
	reply; *_length is the size of \a data, and the length of the reply on
	return.
*/
status_t
BrcmfPcie::Dcmd(uint32 command, bool set, void* data, size_t* _length)
{
	(void)set;	// the msgbuf protocol does not tell gets and sets apart
	if (!fMsgbufReady)
		return B_NO_INIT;

	MutexLocker ioctlLocker(fIoctlLock);

	size_t length = min_c(*_length, kIoctlRequestSize);
	if (data != NULL)
		memcpy(fIoctlRequest.data, data, length);
	else
		memset(fIoctlRequest.data, 0, length);

	ConditionVariableEntry entry;
	{
		MutexLocker locker(fRingLock);
		msgbuf_ioctl_request* request
			= (msgbuf_ioctl_request*)_RingReserve(fControlSubmit);
		if (request == NULL)
			return B_BUSY;

		fIoctlTransaction++;
		fIoctlDone = false;
		fIoctlReplyLength = 0;
		fIoctlCondition.Add(&entry);

		request->header.type = MSGBUF_TYPE_IOCTLPTR_REQ;
		request->header.request_id = kIoctlRequestId;
		request->command = command;
		request->transaction = fIoctlTransaction;
		request->input_length = length;
		request->output_length = min_c(*_length, kControlBufferSize);
		request->request_buffer.low
			= (uint32)(fIoctlRequest.physical & 0xffffffff);
		request->request_buffer.high
			= (uint32)((uint64)fIoctlRequest.physical >> 32);
		_RingCommit(fControlSubmit);
	}

	status_t status = entry.Wait(B_RELATIVE_TIMEOUT, kIoctlTimeout);

	MutexLocker locker(fRingLock);
	if (!fIoctlDone) {
		ERROR("command %" B_PRIu32 " timed out (%s); %" B_PRId32
			" interrupts so far\n", command, strerror(status),
			atomic_get(&fInterruptCount));
		fIoctlDone = true;
			// a late reply is then dropped
		return B_TIMED_OUT;
	}

	size_t replyLength = min_c(fIoctlReplyLength, *_length);
	if (data != NULL)
		memcpy(data, fIoctlReply, replyLength);
	*_length = replyLength;

	if (fIoctlStatus != 0) {
		TRACE("command %" B_PRIu32 " failed: firmware status %d\n", command,
			fIoctlStatus);
		return B_ERROR;
	}
	return B_OK;
}


status_t
BrcmfPcie::GetVar(const char* name, void* data, size_t length)
{
	size_t nameLength = strlen(name) + 1;
	size_t total = max_c(nameLength, length);
	uint8* buffer = (uint8*)calloc(1, total);
	if (buffer == NULL)
		return B_NO_MEMORY;
	memcpy(buffer, name, nameLength);

	size_t replyLength = total;
	status_t status = Dcmd(kDcmdGetVar, false, buffer, &replyLength);
	if (status == B_OK)
		memcpy(data, buffer, min_c(length, replyLength));
	free(buffer);
	return status;
}


status_t
BrcmfPcie::TransportDcmd(uint32 command, bool set, void* data, size_t length,
	size_t* _replyLength)
{
	// Dcmd() always copies the reply back; a set must not see it.
	uint8* buffer = (uint8*)malloc(max_c(length, (size_t)4));
	if (buffer == NULL)
		return B_NO_MEMORY;
	if (data != NULL && length > 0)
		memcpy(buffer, data, length);
	else
		memset(buffer, 0, max_c(length, (size_t)4));

	size_t replyLength = length;
	status_t status = Dcmd(command, set, buffer, &replyLength);
	if (status == B_OK && !set && data != NULL)
		memcpy(data, buffer, min_c(length, replyLength));
	if (_replyLength != NULL)
		*_replyLength = status == B_OK ? replyLength : 0;
	free(buffer);
	return status;
}


/*!	Phase 2 check: asks the firmware for its MAC address and version. */
void
BrcmfPcie::_QueryFirmware()
{
	bigtime_t start = system_time();

	uint8 mac[6] = {};
	status_t status = GetVar("cur_etheraddr", mac, sizeof(mac));
	if (status == B_OK) {
		INFO("MAC address %02x:%02x:%02x:%02x:%02x:%02x (in %" B_PRId64
			" us)\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
			system_time() - start);
	} else
		ERROR("cannot read the MAC address: %s\n", strerror(status));

	char version[256] = {};
	status = GetVar("ver", version, sizeof(version) - 1);
	if (status == B_OK) {
		// one line, without the trailing newline
		char* end = strchr(version, '\n');
		if (end != NULL)
			*end = '\0';
		INFO("firmware version: %s\n", version);
	} else
		ERROR("cannot read the firmware version: %s\n", strerror(status));

	INFO("%" B_PRId32 " interrupts, %" B_PRId32 " messages found by "
		"polling, %" B_PRId32 " events\n", atomic_get(&fInterruptCount),
		atomic_get(&fPolledMessages), fEventCount);
	_DumpConsole();
}
