/*
 * Copyright 2026, RenkuOS Contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */
#ifndef APPLE_SPI_DEVICE_H
#define APPLE_SPI_DEVICE_H


#include <ACPI.h>
#include <lock.h>
#include <spi.h>

#include "AppleSPIProtocol.h"


class AppleSPITouchpad;
class HIDDevice;


//!	The APP000D device: one SPI peripheral carrying keyboard and trackpad.
//	A thread reads a packet whenever the device raises its GPE, puts the
//	packets back together into messages and hands them to the keyboard or
//	the trackpad; it also sends the few commands the device takes, one at a
//	time, each after the device answered the one before.
class AppleSPIDevice {
public:
								AppleSPIDevice(device_node* node,
									spi_device_interface* spi,
									spi_device spiCookie,
									acpi_module_info* acpi,
									acpi_handle handle);
								~AppleSPIDevice();

			status_t			InitCheck() const { return fStatus; }

			device_node*		Node() const { return fNode; }
			HIDDevice*			Keyboard() const { return fKeyboard; }
			HIDDevice*			MediaKeys() const { return fMediaKeys; }
			AppleSPITouchpad*	Touchpad() const { return fTouchpad; }

			bool				IsPublished() const { return fPublished; }
			void				SetPublished() { fPublished = true; }

			void				Removed();
			bool				IsRemoved() const { return fRemoved; }

			// open descriptors; the object goes away with the last one
			// once the device is removed
			void				AcquireReference();
			bool				ReleaseReference();

			void				SetCapsLockLED(bool on);

private:
			status_t			_Init();
			bool				_UsbInterfaceActive();
			void				_ReadAppleProperties();
			status_t			_EvaluateInteger(const char* method,
									uint64* value);
			status_t			_CallMethod(const char* method,
									uint64 argument);
			status_t			_InstallGPE();
			void				_Stop();

	static	uint32				_GPEHandler(acpi_handle gpeDevice,
									uint32 gpeNumber, void* context);
	static	status_t			_ThreadEntry(void* data);
			void				_Run();

			status_t			_ReadPacket();
			status_t			_WriteMessage(uint8 device, uint16 type,
									uint16 responseLength,
									const uint8* payload, size_t length);
			bool				_SendNextCommand();

			void				_HandlePacket();
			void				_HandleMessage(uint8 flags, uint8 device,
									const uint8* message, size_t length);
			void				_HandleKeyboard(const uint8* payload,
									size_t length);
			void				_SetBacklight(int32 step);
			uint16				_BacklightLevel() const;
			void				_NoteActivity();
			void				_CheckBacklightIdle();

private:
	enum {
		kCommandTouchpadInfo	= 0x01,
		kCommandMultitouch		= 0x02,
		kCommandCapsLock		= 0x04,
		kCommandBacklight		= 0x08,
	};

			status_t			fStatus;
			device_node*		fNode;
			spi_device_interface* fSPI;
			spi_device			fSPICookie;
			acpi_module_info*	fACPI;
			acpi_handle			fHandle;

			HIDDevice*			fKeyboard;
			HIDDevice*			fMediaKeys;
			AppleSPITouchpad*	fTouchpad;
			bool				fPublished;
			bool				fRemoved;
			int32				fReferences;

			spi_device_config	fConfig;
			uint32				fCSDelay;		// us

			int32				fGPE;			// -1: polling
			int32				fGPEFired;
			sem_id				fEvent;
			thread_id			fThread;
			bool				fStopping;

			// all below belongs to the thread
			uint8				fReadBuffer[APPLE_SPI_PACKET_SIZE];
			uint8				fWriteBuffer[APPLE_SPI_PACKET_SIZE];
			uint8				fWriteStatus[APPLE_SPI_STATUS_SIZE];
			uint8				fMessage[APPLE_SPI_MAX_MESSAGE];
			size_t				fMessageLength;
			uint8				fMessageCounter;
			bool				fWriteInFlight;
			bigtime_t			fWriteTime;
			uint32				fBadPackets;
			uint32				fLoggedMessages;	// first few, for debugging

			// commands asked for by other threads
			int32				fPendingCommands;
			int32				fCapsLock;

			// keyboard backlight, set from the keyboard by the thread
			int32				fBacklight;		// step, 0 is off
			uint8				fBacklightKeys;	// F5/F6 held down
			uint8				fMediaKey;		// consumer usage held down
			bigtime_t			fBacklightTimeout;	// 0: never idle
			bigtime_t			fLastActivity;
			bool				fBacklightIdle;	// off until the next input
};


#endif // APPLE_SPI_DEVICE_H
