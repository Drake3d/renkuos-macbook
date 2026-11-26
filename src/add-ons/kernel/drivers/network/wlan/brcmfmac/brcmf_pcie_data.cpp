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
 * The data path of the Broadcom FullMAC PCIe chips: buffers posted for
 * received frames, a flow ring for frames to send, and their completions.
 *
 * Provenance: the message formats, the flow ring parameters (512 items of
 * 48 bytes, ring id = flow id + 2, tid from the priority) and the TX post
 * layout (Ethernet header in the message, the rest in the buffer) follow
 * OpenBSD's bwfm(4) PCIe attachment (sys/dev/pci/if_bwfm_pci.c 1.79:
 * bwfm_pci_fill_rx_buf_ring(), bwfm_pci_flowring_create_cb(),
 * bwfm_pci_txdata(), bwfm_pci_msg_rx()), Patrick Wildt and Broadcom
 * Corporation, ISC. Linux brcmfmac compared only.
 *
 * As bwfm for a station, there is a flow ring per FIFO (access category),
 * chosen from the frame's priority (IP precedence), opened with the first
 * frame for it; ring id = FIFO + 2. The rings are deleted when the link
 * goes down, so that the next network gets new ones. Frames are copied
 * into driver-owned buffers instead of mapping the network buffers.
 */


#include "brcmf_pcie.h"

#include <stdlib.h>
#include <string.h>

#include <KernelExport.h>

#include "brcmfmac.h"


extern net_buffer_module_info* gBufferModule;


enum {
	MSGBUF_TYPE_FLOW_RING_CREATE	= 0x03,
	MSGBUF_TYPE_FLOW_RING_DELETE	= 0x05,
	MSGBUF_TYPE_TX_POST				= 0x0f,
	MSGBUF_TYPE_RXBUF_POST			= 0x11,
};

enum {
	FLOW_CLOSED = 0,
	FLOW_OPENING,
	FLOW_OPEN,
	FLOW_CLOSING,
};

static const uint16 kFlowRingItems = 512;
static const uint16 kFlowRingItemSize = 48;
static const uint16 kFlowRingIdBase = 2;	// after the two common rings
static const uint8 kTxFlag8023 = 1 << 0;
static const uint8 kTxPriorityShift = 5;

// 802.1D priority to FIFO, as bwfm_pci_prio2fifo
static const uint8 kPriorityToFifo[8] = { 0, 1, 1, 0, 2, 2, 3, 3 };
static const bigtime_t kFlowTimeout = 2000000;
static const bigtime_t kTxWaitTimeout = 500000;
static const size_t kEthernetHeaderSize = 14;


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

struct msgbuf_completion_header {
	uint16	status;
	uint16	flow_ring;
} _PACKED;

struct msgbuf_rx_post {
	msgbuf_common_header header;
	uint16	metadata_length;
	uint16	data_length;
	uint32	reserved;
	msgbuf_address metadata;
	msgbuf_address data;
} _PACKED;

struct msgbuf_rx_complete {
	msgbuf_common_header header;
	msgbuf_completion_header completion;
	uint16	metadata_length;
	uint16	data_length;
	uint16	data_offset;
	uint16	flags;
	uint32	status0;
	uint32	status1;
	uint32	reserved;
} _PACKED;

struct msgbuf_tx_post {
	msgbuf_common_header header;
	uint8	ethernet_header[14];
	uint8	flags;
	uint8	segments;
	msgbuf_address metadata;
	msgbuf_address data;
	uint16	metadata_length;
	uint16	data_length;
	uint32	reserved;
} _PACKED;

struct msgbuf_tx_status {
	msgbuf_common_header header;
	msgbuf_completion_header completion;
	uint16	metadata_length;
	uint16	status;
} _PACKED;

struct msgbuf_flow_ring_create {
	msgbuf_common_header header;
	uint8	destination[6];
	uint8	source[6];
	uint8	tid;
	uint8	interface_flags;
	uint16	flow_ring;
	uint8	tc;
	uint8	priority;
	uint16	interrupt_vector;
	uint16	max_items;
	uint16	item_length;
	msgbuf_address ring;
} _PACKED;

struct msgbuf_flow_ring_delete {
	msgbuf_common_header header;
	uint16	flow_ring;
	uint16	reason;
	uint32	reserved[7];
} _PACKED;

struct msgbuf_flow_ring_response {
	msgbuf_common_header header;
	msgbuf_completion_header completion;
	uint32	reserved[3];
} _PACKED;

static_assert(sizeof(msgbuf_rx_post) == 32, "RX post size");
static_assert(sizeof(msgbuf_rx_complete) == 32, "RX complete size");
static_assert(sizeof(msgbuf_tx_post) == 48, "TX post size");
static_assert(sizeof(msgbuf_tx_status) == 16, "TX status size");
static_assert(sizeof(msgbuf_flow_ring_create) == 40, "flow ring size");
static_assert(sizeof(msgbuf_flow_ring_delete) == 40, "flow delete size");


/*!	The 802.1D priority of an Ethernet frame, as ieee80211_classify(): the
	IP precedence for IPv4 and IPv6, 0 for anything else.
*/
static uint8
frame_priority(const uint8* frame, size_t size)
{
	if (size < kEthernetHeaderSize + 2)
		return 0;
	uint16 type = (frame[12] << 8) | frame[13];
	if (type == 0x0800)
		return frame[15] >> 5;
	if (type == 0x86dd)
		return (frame[14] & 0x0f) >> 1;
			// traffic class bits 7-5 are bits 3-1 of the first byte
	return 0;
}


static inline void
set_address(msgbuf_address& address, phys_addr_t physical)
{
	address.low = (uint32)(physical & 0xffffffff);
	address.high = (uint32)((uint64)physical >> 32);
}


// #pragma mark - buffers


status_t
BrcmfPcie::_AllocFramePool(brcmf_frame_pool& pool, const char* name)
{
	for (int i = 0; i < BRCMF_PCIE_FRAME_CHUNKS; i++) {
		status_t status = _AllocDma(pool.chunks[i],
			BRCMF_PCIE_FRAMES_PER_CHUNK * BRCMF_PCIE_FRAME_SIZE, name);
		if (status != B_OK)
			return status;
	}
	memset(pool.busy, 0, sizeof(pool.busy));
	pool.busyCount = 0;
	return B_OK;
}


void
BrcmfPcie::_FreeFramePool(brcmf_frame_pool& pool)
{
	for (int i = 0; i < BRCMF_PCIE_FRAME_CHUNKS; i++)
		_FreeDma(pool.chunks[i]);
	memset(pool.busy, 0, sizeof(pool.busy));
	pool.busyCount = 0;
}


status_t
BrcmfPcie::_SetupData()
{
	status_t status = _AllocFramePool(fRxPool, "brcmfmac rx frames");
	if (status == B_OK)
		status = _AllocFramePool(fTxPool, "brcmfmac tx frames");
	if (status != B_OK)
		return status;

	for (int i = 0; i < BRCMF_PCIE_FLOW_RINGS; i++)
		fFlowState[i] = FLOW_CLOSED;
	_PostRxBuffers();
	INFO("%" B_PRId32 " RX buffers posted\n", fRxPool.busyCount);
	return B_OK;
}


void
BrcmfPcie::_FreeData()
{
	_FreeFramePool(fRxPool);
	_FreeFramePool(fTxPool);
	for (int i = 0; i < BRCMF_PCIE_FLOW_RINGS; i++) {
		_FreeDma(fFlowRings[i].memory);
		fFlowState[i] = FLOW_CLOSED;
	}
	fTxCondition.NotifyAll();
	fFlowCondition.NotifyAll();
}


/*!	Gives the firmware free RX buffers, up to the number it accepts. The
	request id is the buffer's index.
*/
void
BrcmfPcie::_PostRxBuffers()
{
	MutexLocker locker(fRingLock);
	if (fRxPool.chunks[0].area < 0)
		return;

	int32 limit = min_c((int32)fMaxRxBufPost, (int32)BRCMF_PCIE_FRAMES);
	bool posted = false;
	for (int32 i = 0; i < BRCMF_PCIE_FRAMES && fRxPool.busyCount < limit;
			i++) {
		if (fRxPool.busy[i])
			continue;
		msgbuf_rx_post* post = (msgbuf_rx_post*)_RingReserve(fRxPostSubmit);
		if (post == NULL)
			break;
		post->header.type = MSGBUF_TYPE_RXBUF_POST;
		post->header.request_id = i;
		post->data_length = BRCMF_PCIE_FRAME_SIZE;
		set_address(post->data, fRxPool.Physical(i));
		fRxPool.busy[i] = true;
		fRxPool.busyCount++;
		posted = true;
	}
	if (posted)
		_RingCommit(fRxPostSubmit);
}


// #pragma mark - completions


void
BrcmfPcie::_HandleRxComplete(const uint8* message)
{
	const msgbuf_rx_complete* complete = (const msgbuf_rx_complete*)message;
	uint32 index = complete->header.request_id;
	if (index >= BRCMF_PCIE_FRAMES || !fRxPool.busy[index]) {
		ERROR("RX complete for unknown buffer %" B_PRIu32 "\n", index);
		return;
	}

	uint32 offset = complete->data_offset != 0
		? complete->data_offset : fRxDataOffset;
	uint32 length = complete->data_length;
	if (length > 1000)
		atomic_add(&fRxLarge, 1);
	if ((int32)length > fRxMaxLength)
		fRxMaxLength = length;
	if ((fRxFlagsSeen & (1u << (complete->flags & 31))) == 0) {
		fRxFlagsSeen |= 1u << (complete->flags & 31);
		TRACE("first RX frame with flags %#x: %" B_PRIu32 " bytes at offset %"
			B_PRIu32 ", metadata %u, status %#" B_PRIx32 "/%#" B_PRIx32 "\n",
			complete->flags, length, offset, complete->metadata_length,
			complete->status0, complete->status1);
	}
	if (offset + length <= BRCMF_PCIE_FRAME_SIZE && fReceiver != NULL) {
		fReceiver->ReceiveData(fRxPool.Data(index) + offset, length);
		atomic_add(&fRxFrames, 1);
		atomic_add64(&fRxBytes, length);
	} else if (offset + length > BRCMF_PCIE_FRAME_SIZE) {
		ERROR("RX frame of %" B_PRIu32 " bytes at %" B_PRIu32
			" does not fit\n", length, offset);
	}

	MutexLocker locker(fRingLock);
	fRxPool.busy[index] = false;
	fRxPool.busyCount--;
}


void
BrcmfPcie::_HandleTxStatus(const uint8* message)
{
	const msgbuf_tx_status* status = (const msgbuf_tx_status*)message;
	// request ids are buffer index + 1, as bwfm
	uint32 index = status->header.request_id - 1;
	if (index >= BRCMF_PCIE_FRAMES) {
		ERROR("TX status for unknown buffer %" B_PRIu32 "\n", index + 1);
		return;
	}
	if (status->status != 0)
		atomic_add(&fTxFailed, 1);

	MutexLocker locker(fRingLock);
	if (fTxPool.busy[index]) {
		fTxPool.busy[index] = false;
		fTxPool.busyCount--;
	}
	locker.Unlock();
	fTxCondition.NotifyAll();
}


void
BrcmfPcie::_HandleFlowRingCreated(const uint8* message)
{
	const msgbuf_flow_ring_response* response
		= (const msgbuf_flow_ring_response*)message;
	int32 fifo = (int32)response->completion.flow_ring - kFlowRingIdBase;
	if (fifo < 0 || fifo >= BRCMF_PCIE_FLOW_RINGS) {
		ERROR("unexpected flow ring %u created\n",
			response->completion.flow_ring);
		return;
	}

	MutexLocker locker(fRingLock);
	if (fFlowState[fifo] != FLOW_OPENING)
		return;
	fFlowStatus[fifo] = response->completion.status;
	fFlowState[fifo] = fFlowStatus[fifo] == 0 ? FLOW_OPEN : FLOW_CLOSED;
	locker.Unlock();
	fFlowCondition.NotifyAll();
}


/*!	The firmware let go of a flow ring: frames still queued on it will not
	be sent, so their buffers are free again.
*/
void
BrcmfPcie::_HandleFlowRingDeleted(const uint8* message)
{
	const msgbuf_flow_ring_response* response
		= (const msgbuf_flow_ring_response*)message;
	int32 fifo = (int32)response->completion.flow_ring - kFlowRingIdBase;
	if (fifo < 0 || fifo >= BRCMF_PCIE_FLOW_RINGS) {
		ERROR("unexpected flow ring %u deleted\n",
			response->completion.flow_ring);
		return;
	}
	if (response->completion.status != 0) {
		ERROR("deleting flow ring %" B_PRId32 " failed: status %u\n", fifo,
			response->completion.status);
	}

	MutexLocker locker(fRingLock);
	int32 freed = 0;
	for (int32 i = 0; i < BRCMF_PCIE_FRAMES; i++) {
		if (fTxPool.busy[i] && fTxRing[i] == fifo) {
			fTxPool.busy[i] = false;
			fTxPool.busyCount--;
			freed++;
		}
	}
	fFlowState[fifo] = FLOW_CLOSED;
	locker.Unlock();

	TRACE("flow ring %" B_PRId32 " deleted, %" B_PRId32 " frames dropped\n",
		fifo, freed);
	fFlowCondition.NotifyAll();
	fTxCondition.NotifyAll();
}


/*!	Deletes every open flow ring, as bwfm_pci_flowring_delete(), without
	waiting: TransportSend() waits for a ring that is still closing.
*/
void
BrcmfPcie::TransportLinkDown()
{
	if (!fMsgbufReady)
		return;

	MutexLocker locker(fRingLock);
	bool sent = false;
	for (int32 fifo = 0; fifo < BRCMF_PCIE_FLOW_RINGS; fifo++) {
		if (fFlowState[fifo] != FLOW_OPEN)
			continue;
		msgbuf_flow_ring_delete* request
			= (msgbuf_flow_ring_delete*)_RingReserve(fControlSubmit);
		if (request == NULL) {
			ERROR("cannot delete flow ring %" B_PRId32 "\n", fifo);
			continue;
		}
		request->header.type = MSGBUF_TYPE_FLOW_RING_DELETE;
		request->flow_ring = fifo + kFlowRingIdBase;
		fFlowState[fifo] = FLOW_CLOSING;
		sent = true;
	}
	if (sent) {
		_RingCommit(fControlSubmit);
		INFO("link down: flow rings closed\n");
	}
}


// #pragma mark - sending


/*!	Opens the flow ring of \a fifo, as bwfm_pci_flowring_create_cb(), and
	waits for the firmware to confirm it. Called with the TX lock held.
*/
status_t
BrcmfPcie::_OpenFlowRing(int32 fifo, const uint8* destination,
	const uint8* source)
{
	brcmf_msgring& ring = fFlowRings[fifo];
	if (ring.memory.area < 0) {
		status_t status = _AllocDma(ring.memory,
			(size_t)kFlowRingItems * kFlowRingItemSize, "brcmfmac flow ring");
		if (status != B_OK)
			return status;
	}

	const uint32 stride = sizeof(uint32);
	uint16 ringId = fifo + kFlowRingIdBase;
	ring.name = "flow";
	ring.writeIndexAddress = fH2DWriteBase + ringId * stride;
	ring.readIndexAddress = fH2DReadBase + ringId * stride;
	ring.writePointer = 0;
	ring.readPointer = 0;
	ring.items = kFlowRingItems;
	ring.itemSize = kFlowRingItemSize;
	memset(ring.memory.data, 0, ring.memory.size);
	_TcmWrite2(ring.readIndexAddress, 0);
	_TcmWrite2(ring.writeIndexAddress, 0);

	ConditionVariableEntry entry;
	{
		MutexLocker locker(fRingLock);
		msgbuf_flow_ring_create* create
			= (msgbuf_flow_ring_create*)_RingReserve(fControlSubmit);
		if (create == NULL)
			return B_BUSY;

		fFlowState[fifo] = FLOW_OPENING;
		fFlowCondition.Add(&entry);

		create->header.type = MSGBUF_TYPE_FLOW_RING_CREATE;
		memcpy(create->destination, destination, 6);
		memcpy(create->source, source, 6);
		create->tid = fifo;
		create->flow_ring = ringId;
		create->max_items = kFlowRingItems;
		create->item_length = kFlowRingItemSize;
		set_address(create->ring, ring.memory.physical);
		_RingCommit(fControlSubmit);
	}

	bigtime_t timeout = system_time() + kFlowTimeout;
	while (true) {
		entry.Wait(B_ABSOLUTE_TIMEOUT, timeout);
		MutexLocker locker(fRingLock);
		if (fFlowState[fifo] == FLOW_OPEN) {
			INFO("flow ring %" B_PRId32 " open\n", fifo);
			return B_OK;
		}
		if (fFlowState[fifo] == FLOW_CLOSED) {
			ERROR("flow ring %" B_PRId32 " creation failed: status %u\n",
				fifo, fFlowStatus[fifo]);
			return B_ERROR;
		}
		if (system_time() >= timeout) {
			ERROR("no answer to the creation of flow ring %" B_PRId32 "\n",
				fifo);
			fFlowState[fifo] = FLOW_CLOSED;
			return B_TIMED_OUT;
		}
		fFlowCondition.Add(&entry);
	}
}


status_t
BrcmfPcie::TransportSend(net_buffer* buffer)
{
	if (!fMsgbufReady)
		return B_DEV_NOT_READY;
	size_t size = buffer->size;
	if (size < kEthernetHeaderSize || size > BRCMF_PCIE_FRAME_SIZE)
		return B_BAD_VALUE;

	MutexLocker txLocker(fTxLock);

	// find a free buffer
	int32 index = -1;
	bigtime_t timeout = system_time() + kTxWaitTimeout;
	while (true) {
		ConditionVariableEntry entry;
		{
			MutexLocker locker(fRingLock);
			if (fTxPool.chunks[0].area < 0)
				return B_DEV_NOT_READY;
			for (int32 i = 0; i < BRCMF_PCIE_FRAMES; i++) {
				if (!fTxPool.busy[i]) {
					index = i;
					break;
				}
			}
			if (index < 0)
				fTxCondition.Add(&entry);
		}
		if (index >= 0)
			break;
		if (entry.Wait(B_ABSOLUTE_TIMEOUT, timeout) == B_TIMED_OUT
			&& system_time() >= timeout) {
			return B_BUSY;
		}
	}

	uint8* data = fTxPool.Data(index);
	if (gBufferModule->read(buffer, 0, data, size) != B_OK)
		return B_BAD_DATA;

	uint8 priority = frame_priority(data, size);
	int32 fifo = kPriorityToFifo[priority];

	// wait for a ring that is still being deleted, then (re)open it
	timeout = system_time() + kFlowTimeout;
	while (true) {
		ConditionVariableEntry entry;
		{
			MutexLocker locker(fRingLock);
			if (fFlowState[fifo] != FLOW_CLOSING)
				break;
			fFlowCondition.Add(&entry);
		}
		if (entry.Wait(B_ABSOLUTE_TIMEOUT, timeout) == B_TIMED_OUT
			&& system_time() >= timeout) {
			ERROR("flow ring %" B_PRId32 " is not closing\n", fifo);
			MutexLocker locker(fRingLock);
			fFlowState[fifo] = FLOW_CLOSED;
			break;
		}
	}
	if (fFlowState[fifo] != FLOW_OPEN) {
		status_t status = _OpenFlowRing(fifo, data, data + 6);
		if (status != B_OK)
			return status;
	}

	brcmf_msgring& ring = fFlowRings[fifo];
	MutexLocker locker(fRingLock);
	if (fFlowState[fifo] != FLOW_OPEN)
		return B_BUSY;
	msgbuf_tx_post* post = (msgbuf_tx_post*)_RingReserve(ring);
	if (post == NULL)
		return B_BUSY;

	post->header.type = MSGBUF_TYPE_TX_POST;
	post->header.request_id = index + 1;
	memcpy(post->ethernet_header, data, kEthernetHeaderSize);
	post->flags = kTxFlag8023 | (priority << kTxPriorityShift);
	post->segments = 1;
	post->data_length = size - kEthernetHeaderSize;
	set_address(post->data, fTxPool.Physical(index) + kEthernetHeaderSize);
	fTxPool.busy[index] = true;
	fTxPool.busyCount++;
	fTxRing[index] = fifo;
	_RingCommit(ring);
	locker.Unlock();

	atomic_add(&fTxFrames, 1);
	atomic_add64(&fTxBytes, size);
	gBufferModule->free(buffer);
	return B_OK;
}
