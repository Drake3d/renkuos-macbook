/*
 * Copyright 2026, RenkuOS Contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */
#ifndef APPLE_SPI_HID_DEVICE_H
#define APPLE_SPI_HID_DEVICE_H


#include <lock.h>

#include "HIDParser.h"


class AppleSPIDevice;
class ProtocolHandler;


#define APPLE_SPI_BOOT_REPORT_SIZE	8
#define APPLE_SPI_MEDIA_REPORT_SIZE	1
#define APPLE_SPI_REPORT_QUEUE		32


enum apple_spi_hid_kind {
	APPLE_SPI_HID_KEYBOARD,
	APPLE_SPI_HID_MEDIA_KEYS
};


//!	The keyboard, presented to the shared HID code (the same code the USB
//	and I2C HID drivers use) as a HID boot keyboard, and its media keys as
//	a second, consumer control device. The Apple reports are turned into
//	these reports by AppleSPIDevice and queued here; the caps lock LED
//	report goes back to AppleSPIDevice as a command.
//	The class name and methods are the interface the shared code expects.
class HIDDevice {
public:
								HIDDevice(AppleSPIDevice* transport,
									apple_spi_hid_kind kind);
								~HIDDevice();

			status_t			InitCheck() const { return fStatus; }

			bool				IsOpen() const { return fOpenCount > 0; }
			status_t			Open(ProtocolHandler* handler, uint32 flags);
			status_t			Close(ProtocolHandler* handler);
			int32				OpenCount() const { return fOpenCount; }

			void				Removed();
			bool				IsRemoved() const { return fRemoved; }

			status_t			MaybeScheduleTransfer(HIDReport* report);
			status_t			SendReport(HIDReport* report);

			HIDParser&			Parser() { return fParser; }
			ProtocolHandler*	ProtocolHandlerAt(uint32 index) const;

			// from the transport
			void				QueueReport(const uint8* report);

private:
			status_t			fStatus;
			AppleSPIDevice*		fTransport;
			apple_spi_hid_kind	fKind;
			size_t				fReportSize;

			int32				fOpenCount;
			bool				fRemoved;

			HIDParser			fParser;
			uint32				fProtocolHandlerCount;
			ProtocolHandler*	fProtocolHandlerList;

			// the report the shared code is reading
			uint8				fTransferBuffer[APPLE_SPI_BOOT_REPORT_SIZE];

			mutex				fQueueLock;
			bool				fTransferPending;
			uint8				fQueue[APPLE_SPI_REPORT_QUEUE]
									[APPLE_SPI_BOOT_REPORT_SIZE];
			uint32				fQueueHead;
			uint32				fQueueCount;
};


#endif // APPLE_SPI_HID_DEVICE_H
