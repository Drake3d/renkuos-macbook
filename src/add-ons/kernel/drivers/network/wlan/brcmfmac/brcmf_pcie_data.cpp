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
 * Simplifications for a station: one flow ring (priority 0) carries every
 * frame, opened with the first frame sent; frames are copied into
 * driver-owned buffers instead of mapping the network buffers.
 */


#include "brcmf_pcie.h"

#include <stdlib.h>
#include <string.h>

#include <KernelExport.h>

#include "brcmfmac.h"


extern net_buffer_module_info* gBufferModule;


enum {
	MSGBUF_TYPE_FLOW_RING_CREATE	= 0x03,
	MSGBUF_TYPE_TX_POST				= 0x0f,
	MSGBUF_TYPE_RXBUF_POST			= 0x11,
};

enum {
	FLOW_CLOSED = 0,
	FLOW_OPENING,
	FLOW_OPEN,
	FLOW_FAILED,
};

static const uint16 kFlowRingItems = 512;
static const uint16 kFlowRingItemSize = 48;
static const uint16 kFlowId = 0;			// ring id kFlowId + 2
static const uint8 kTxFlag8023 = 1 << 0;
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

	fFlowState = FLOW_CLOSED;
	_PostRxBuffers();
	INFO("%" B_PRId32 " RX buffers posted\n", fRxPool.busyCount);
	return B_OK;
}


void
BrcmfPcie::_FreeData()
{
	_FreeFramePool(fRxPool);
	_FreeFramePool(fTxPool);
	_FreeDma(fFlowRing.memory);
	fFlowState = FLOW_CLOSED;
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
	if (offset + length <= BRCMF_PCIE_FRAME_SIZE && fReceiver != NULL) {
		fReceiver->ReceiveData(fRxPool.Data(index) + offset, length);
		atomic_add(&fRxFrames, 1);
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
	if (response->completion.flow_ring != kFlowId + 2) {
		ERROR("unexpected flow ring %u created\n",
			response->completion.flow_ring);
		return;
	}

	MutexLocker locker(fRingLock);
	fFlowStatus = response->completion.status;
	fFlowState = fFlowStatus == 0 ? FLOW_OPEN : FLOW_FAILED;
	locker.Unlock();
	fFlowCondition.NotifyAll();
}


// #pragma mark - sending


/*!	Opens the flow ring, as bwfm_pci_flowring_create_cb(), and waits for the
	firmware to confirm it. Called with the TX lock held.
*/
status_t
BrcmfPcie::_OpenFlowRing(const uint8* destination, const uint8* source)
{
	if (fFlowRing.memory.area < 0) {
		status_t status = _AllocDma(fFlowRing.memory,
			(size_t)kFlowRingItems * kFlowRingItemSize, "brcmfmac flow ring");
		if (status != B_OK)
			return status;
	}

	const uint32 stride = sizeof(uint32);
	fFlowRing.name = "flow";
	fFlowRing.writeIndexAddress = fH2DWriteBase + (kFlowId + 2) * stride;
	fFlowRing.readIndexAddress = fH2DReadBase + (kFlowId + 2) * stride;
	fFlowRing.writePointer = 0;
	fFlowRing.readPointer = 0;
	fFlowRing.items = kFlowRingItems;
	fFlowRing.itemSize = kFlowRingItemSize;
	memset(fFlowRing.memory.data, 0, fFlowRing.memory.size);
	_TcmWrite2(fFlowRing.readIndexAddress, 0);
	_TcmWrite2(fFlowRing.writeIndexAddress, 0);

	ConditionVariableEntry entry;
	{
		MutexLocker locker(fRingLock);
		msgbuf_flow_ring_create* create
			= (msgbuf_flow_ring_create*)_RingReserve(fControlSubmit);
		if (create == NULL)
			return B_BUSY;

		fFlowState = FLOW_OPENING;
		fFlowCondition.Add(&entry);

		create->header.type = MSGBUF_TYPE_FLOW_RING_CREATE;
		memcpy(create->destination, destination, 6);
		memcpy(create->source, source, 6);
		create->tid = 0;
		create->flow_ring = kFlowId + 2;
		create->max_items = kFlowRingItems;
		create->item_length = kFlowRingItemSize;
		set_address(create->ring, fFlowRing.memory.physical);
		_RingCommit(fControlSubmit);
	}

	entry.Wait(B_RELATIVE_TIMEOUT, kFlowTimeout);

	MutexLocker locker(fRingLock);
	if (fFlowState == FLOW_OPEN) {
		INFO("flow ring open\n");
		return B_OK;
	}
	if (fFlowState == FLOW_OPENING) {
		ERROR("no answer to the flow ring creation\n");
		fFlowState = FLOW_CLOSED;
		return B_TIMED_OUT;
	}
	ERROR("flow ring creation failed: status %u\n", fFlowStatus);
	fFlowState = FLOW_CLOSED;
	return B_ERROR;
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
		}
		if (index >= 0)
			break;
		if (system_time() > timeout)
			return B_BUSY;
		ConditionVariableEntry entry;
		fTxCondition.Add(&entry);
		entry.Wait(B_RELATIVE_TIMEOUT, 20000);
	}

	uint8* data = fTxPool.Data(index);
	if (gBufferModule->read(buffer, 0, data, size) != B_OK)
		return B_BAD_DATA;

	if (fFlowState != FLOW_OPEN) {
		status_t status = _OpenFlowRing(data, data + 6);
		if (status != B_OK)
			return status;
	}

	msgbuf_tx_post* post = (msgbuf_tx_post*)_RingReserve(fFlowRing);
	if (post == NULL)
		return B_BUSY;

	post->header.type = MSGBUF_TYPE_TX_POST;
	post->header.request_id = index + 1;
	memcpy(post->ethernet_header, data, kEthernetHeaderSize);
	post->flags = kTxFlag8023;
		// priority 0
	post->segments = 1;
	post->data_length = size - kEthernetHeaderSize;
	set_address(post->data, fTxPool.Physical(index) + kEthernetHeaderSize);
	{
		MutexLocker locker(fRingLock);
		fTxPool.busy[index] = true;
		fTxPool.busyCount++;
	}
	_RingCommit(fFlowRing);

	atomic_add(&fTxFrames, 1);
	gBufferModule->free(buffer);
	return B_OK;
}
