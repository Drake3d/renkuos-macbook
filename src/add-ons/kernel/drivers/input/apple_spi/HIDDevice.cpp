/*
 * Copyright 2026, RenkuOS Contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


#include "HIDDevice.h"

#include <string.h>

#include <util/AutoLock.h>

#include "AppleSPIDevice.h"
#include "Driver.h"
#include "HIDReport.h"
#include "ProtocolHandler.h"


// A HID boot keyboard: modifier bits, a reserved byte and six key codes in,
// five LEDs out. The shared keyboard handler turns these into key events,
// key repeat and the LED requests, as it does for USB keyboards.
static const uint8 kBootKeyboardDescriptor[] = {
	0x05, 0x01,			// Usage Page (Generic Desktop)
	0x09, 0x06,			// Usage (Keyboard)
	0xa1, 0x01,			// Collection (Application)
	0x05, 0x07,			//   Usage Page (Keyboard)
	0x19, 0xe0,			//   Usage Minimum (Left Control)
	0x29, 0xe7,			//   Usage Maximum (Right GUI)
	0x15, 0x00,			//   Logical Minimum (0)
	0x25, 0x01,			//   Logical Maximum (1)
	0x75, 0x01,			//   Report Size (1)
	0x95, 0x08,			//   Report Count (8)
	0x81, 0x02,			//   Input (Data, Variable, Absolute)
	0x95, 0x01,			//   Report Count (1)
	0x75, 0x08,			//   Report Size (8)
	0x81, 0x01,			//   Input (Constant)
	0x95, 0x05,			//   Report Count (5)
	0x75, 0x01,			//   Report Size (1)
	0x05, 0x08,			//   Usage Page (LEDs)
	0x19, 0x01,			//   Usage Minimum (Num Lock)
	0x29, 0x05,			//   Usage Maximum (Kana)
	0x91, 0x02,			//   Output (Data, Variable, Absolute)
	0x95, 0x01,			//   Report Count (1)
	0x75, 0x03,			//   Report Size (3)
	0x91, 0x01,			//   Output (Constant)
	0x95, 0x06,			//   Report Count (6)
	0x75, 0x08,			//   Report Size (8)
	0x15, 0x00,			//   Logical Minimum (0)
	0x26, 0xff, 0x00,	//   Logical Maximum (255)
	0x05, 0x07,			//   Usage Page (Keyboard)
	0x19, 0x00,			//   Usage Minimum (0)
	0x2a, 0xff, 0x00,	//   Usage Maximum (255)
	0x81, 0x00,			//   Input (Data, Array)
	0xc0				// End Collection
};


HIDDevice::HIDDevice(AppleSPIDevice* transport)
	:
	fStatus(B_NO_INIT),
	fTransport(transport),
	fOpenCount(0),
	fRemoved(false),
	fParser(this),
	fProtocolHandlerCount(0),
	fProtocolHandlerList(NULL),
	fTransferPending(false),
	fQueueHead(0),
	fQueueCount(0)
{
	mutex_init(&fQueueLock, "apple_spi keyboard queue");

	fStatus = fParser.ParseReportDescriptor(kBootKeyboardDescriptor,
		sizeof(kBootKeyboardDescriptor));
	if (fStatus != B_OK) {
		ERROR("parsing the keyboard descriptor failed: %s\n",
			strerror(fStatus));
		return;
	}

	ProtocolHandler::AddHandlers(*this, fProtocolHandlerList,
		fProtocolHandlerCount);
	if (fProtocolHandlerCount == 0) {
		ERROR("no keyboard handler for the descriptor\n");
		fStatus = B_ERROR;
	}
}


HIDDevice::~HIDDevice()
{
	ProtocolHandler* handler = fProtocolHandlerList;
	while (handler != NULL) {
		ProtocolHandler* next = handler->NextHandler();
		delete handler;
		handler = next;
	}

	mutex_destroy(&fQueueLock);
}


status_t
HIDDevice::Open(ProtocolHandler* handler, uint32 flags)
{
	atomic_add(&fOpenCount, 1);
	return B_OK;
}


status_t
HIDDevice::Close(ProtocolHandler* handler)
{
	if (atomic_add(&fOpenCount, -1) == 1) {
		MutexLocker locker(fQueueLock);
		fQueueCount = 0;
		fTransferPending = false;
	}
	return B_OK;
}


void
HIDDevice::Removed()
{
	fRemoved = true;
}


/*!	Called by the shared code before it waits for a report. Never blocks,
	since the keyboard handler times its key repeat by how long the wait
	takes: a queued report is delivered right away, otherwise the next one
	from the device is delivered when it comes, like a USB interrupt
	transfer would.
*/
status_t
HIDDevice::MaybeScheduleTransfer(HIDReport* report)
{
	if (fRemoved)
		return ENODEV;
	if (report->Type() != HID_REPORT_TYPE_INPUT)
		return B_OK;

	MutexLocker locker(fQueueLock);
	if (fQueueCount == 0) {
		fTransferPending = true;
		return B_OK;
	}

	memcpy(fTransferBuffer, fQueue[fQueueHead], sizeof(fTransferBuffer));
	fQueueHead = (fQueueHead + 1) % APPLE_SPI_REPORT_QUEUE;
	fQueueCount--;
	fTransferPending = false;
	locker.Unlock();

	fParser.SetReport(B_OK, fTransferBuffer, sizeof(fTransferBuffer));
	return B_OK;
}


//!	Takes a boot report from the transport.
void
HIDDevice::QueueReport(const uint8* report)
{
	if (fRemoved || !IsOpen())
		return;

	MutexLocker locker(fQueueLock);
	if (fTransferPending && fQueueCount == 0) {
		// Someone is waiting. As with USB HID, should the wait have timed
		// out just now, the report is lost; the next one carries the full
		// key state again.
		fTransferPending = false;
		memcpy(fTransferBuffer, report, sizeof(fTransferBuffer));
		locker.Unlock();

		fParser.SetReport(B_OK, fTransferBuffer, sizeof(fTransferBuffer));
		return;
	}

	if (fQueueCount == APPLE_SPI_REPORT_QUEUE) {
		// Nobody has read for a long time; keep the newest state.
		fQueueHead = (fQueueHead + 1) % APPLE_SPI_REPORT_QUEUE;
		fQueueCount--;
	}

	uint32 tail = (fQueueHead + fQueueCount) % APPLE_SPI_REPORT_QUEUE;
	memcpy(fQueue[tail], report, APPLE_SPI_BOOT_REPORT_SIZE);
	fQueueCount++;
}


//!	The only output report is the LED state; the keyboard only has caps lock.
status_t
HIDDevice::SendReport(HIDReport* report)
{
	if (fRemoved)
		return ENODEV;
	if (report->Type() != HID_REPORT_TYPE_OUTPUT || report->ReportSize() < 1)
		return B_OK;

	// bit 0 num lock, bit 1 caps lock, as laid out in the descriptor
	fTransport->SetCapsLockLED((report->CurrentReport()[0] & 0x02) != 0);
	return B_OK;
}


ProtocolHandler*
HIDDevice::ProtocolHandlerAt(uint32 index) const
{
	ProtocolHandler* handler = fProtocolHandlerList;
	while (handler != NULL) {
		if (index == 0)
			return handler;

		handler = handler->NextHandler();
		index--;
	}

	return NULL;
}
