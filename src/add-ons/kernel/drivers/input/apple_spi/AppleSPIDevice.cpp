/*
 * Copyright 2026, RenkuOS Contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


#include "AppleSPIDevice.h"

#include <new>
#include <stdlib.h>
#include <string.h>

#include <driver_settings.h>
#include <KernelExport.h>
#include <usb/USB_hid.h>

extern "C" {
#	include "acpi.h"
}

#include "AppleSPITouchpad.h"
#include "Driver.h"
#include "HIDDevice.h"


// How long to wait for the reply to a command before sending the next one.
static const bigtime_t kWriteTimeout = 1000000;
static const int kCommandAttempts = 3;
static const bigtime_t kCommandRetryDelay = 10000;
// How many non-keyboard messages to log, to see the trackpad come up.
static const uint32 kLoggedMessages = 8;
// Without a GPE, the device is read at this rate.
static const bigtime_t kPollInterval = 10000;

// Apple's device properties _DSM, a0b5b7c6-1318-441c-b0c9-fe695eaf949b.
static const uint8 kApplePropertiesUUID[16] = {
	0xc6, 0xb7, 0xb5, 0xa0, 0x18, 0x13, 0x1c, 0x44,
	0xb0, 0xc9, 0xfe, 0x69, 0x5e, 0xaf, 0x94, 0x9b
};


// With fn held, these keys become the keys printed on them in small type.
static const struct {
	uint8	key;
	uint8	fnKey;
} kFnKeys[] = {
	{ 0x2a, 0x4c },		// Backspace -> Delete
	{ 0x28, 0x49 },		// Return -> Insert
	{ 0x50, 0x4a },		// Left -> Home
	{ 0x4f, 0x4d },		// Right -> End
	{ 0x52, 0x4b },		// Up -> Page Up
	{ 0x51, 0x4e },		// Down -> Page Down
};

#define HID_KEY_ERROR_ROLLOVER	0x01
#define HID_KEY_F5				0x3e
#define HID_KEY_F6				0x3f

// F1 and F2 carry the screen brightness symbols, F10 to F12 the mute and
// volume ones. Without fn they are sent as those media keys; with fn they
// are the F keys.
static const struct {
	uint8	key;
	uint8	usage;
} kMediaKeys[] = {
	{ 0x3a, B_HID_UID_CON_DISPLAY_BRIGHTNESS_DECREMENT },	// F1
	{ 0x3b, B_HID_UID_CON_DISPLAY_BRIGHTNESS_INCREMENT },	// F2
	{ 0x43, B_HID_UID_CON_MUTE },				// F10
	{ 0x44, B_HID_UID_CON_VOLUME_DECREMENT },	// F11
	{ 0x45, B_HID_UID_CON_VOLUME_INCREMENT },	// F12
};

// F5 and F6 carry the keyboard backlight symbols. Without fn they dim and
// brighten it in this many steps; with fn they are F5 and F6.
static const int32 kBacklightSteps = 16;
static const int32 kDefaultBacklight = 8;
// The backlight goes off after this long without a key press or a touch,
// and comes back with the next one.
static const bigtime_t kDefaultBacklightTimeout = 30000000;
static const uint8 kBacklightDownKey = 0x01;
static const uint8 kBacklightUpKey = 0x02;


AppleSPIDevice::AppleSPIDevice(device_node* node, spi_device_interface* spi,
	spi_device spiCookie, acpi_module_info* acpi, acpi_handle handle)
	:
	fStatus(B_NO_INIT),
	fNode(node),
	fSPI(spi),
	fSPICookie(spiCookie),
	fACPI(acpi),
	fHandle(handle),
	fKeyboard(NULL),
	fMediaKeys(NULL),
	fTouchpad(NULL),
	fPublished(false),
	fRemoved(false),
	fReferences(0),
	fCSDelay(10),
	fGPE(-1),
	fGPEFired(0),
	fEvent(-1),
	fThread(-1),
	fStopping(false),
	fMessageLength(0),
	fMessageCounter(0),
	fWriteInFlight(false),
	fWriteTime(0),
	fBadPackets(0),
	fLoggedMessages(0),
	fPendingCommands(0),
	fCapsLock(0),
	fBacklight(kDefaultBacklight),
	fBacklightKeys(0),
	fMediaKey(0),
	fBacklightTimeout(kDefaultBacklightTimeout),
	fLastActivity(0),
	fBacklightIdle(false)
{
	fStatus = _Init();
	if (fStatus != B_OK)
		_Stop();
}


AppleSPIDevice::~AppleSPIDevice()
{
	_Stop();
	delete fKeyboard;
	delete fMediaKeys;
	delete fTouchpad;
}


status_t
AppleSPIDevice::_Init()
{
	if (_UsbInterfaceActive())
		return B_NOT_SUPPORTED;

	fKeyboard = new(std::nothrow) HIDDevice(this, APPLE_SPI_HID_KEYBOARD);
	fMediaKeys = new(std::nothrow) HIDDevice(this, APPLE_SPI_HID_MEDIA_KEYS);
	fTouchpad = new(std::nothrow) AppleSPITouchpad();
	if (fKeyboard == NULL || fMediaKeys == NULL || fTouchpad == NULL)
		return B_NO_MEMORY;
	if (fKeyboard->InitCheck() != B_OK)
		return fKeyboard->InitCheck();
	if (fMediaKeys->InitCheck() != B_OK)
		return fMediaKeys->InitCheck();
	if (fTouchpad->InitCheck() != B_OK)
		return fTouchpad->InitCheck();

	status_t status = fSPI->get_config(fSPICookie, &fConfig);
	if (status != B_OK)
		return status;
	_ReadAppleProperties();
	status = fSPI->set_config(fSPICookie, &fConfig);
	if (status != B_OK) {
		ERROR("the controller refused the SPI settings: %s\n",
			strerror(status));
		return status;
	}

	// Route the keyboard and trackpad to SPI. The method is absent where
	// SPI is the only way.
	_CallMethod("SIEN", 1);

	fEvent = create_sem(0, "apple_spi event");
	if (fEvent < 0)
		return fEvent;

	if (_InstallGPE() != B_OK) {
		TRACE_ALWAYS("no GPE, polling every %" B_PRIdBIGTIME " us\n",
			kPollInterval);
	}

	fThread = spawn_kernel_thread(_ThreadEntry, "apple_spi reader",
		B_URGENT_DISPLAY_PRIORITY, this);
	if (fThread < 0)
		return fThread;

	// Driver settings: "keyboard_backlight <0-16>" is the backlight level at
	// start, "keyboard_backlight_timeout <seconds>" how long it stays on
	// without input; 0 keeps it on.
	void* settings = load_driver_settings(DRIVER_NAME);
	if (settings != NULL) {
		const char* value = get_driver_parameter(settings,
			"keyboard_backlight", NULL, NULL);
		if (value != NULL) {
			fBacklight = max_c(0, min_c(strtol(value, NULL, 10),
				kBacklightSteps));
		}
		value = get_driver_parameter(settings, "keyboard_backlight_timeout",
			NULL, NULL);
		if (value != NULL)
			fBacklightTimeout = max_c(0, strtol(value, NULL, 10)) * 1000000LL;
		unload_driver_settings(settings);
	}
	fLastActivity = system_time();
	int32 commands = kCommandTouchpadInfo | kCommandMultitouch
		| kCommandBacklight;

	// Ask what trackpad this is, then put it into multitouch mode. Wake the
	// thread so that this goes out now, not with the first key press.
	atomic_or(&fPendingCommands, commands);
	resume_thread(fThread);
	release_sem(fEvent);

	TRACE_ALWAYS("%" B_PRIu32 " Hz, mode %u, %u bits, chip select delay %"
		B_PRIu32 " us, GPE %" B_PRId32 "\n", fConfig.speed_hz, fConfig.mode,
		fConfig.bits_per_word, fCSDelay, fGPE);
	return B_OK;
}


/*!	Some machines can also connect the keyboard and trackpad over USB. If
	the firmware left them on USB, the USB HID driver serves them already.
	With "prefer_usb true" in the apple_spi driver settings, they are moved
	to USB where the machine allows it; the 2016 and 2017 MacBook Pro do not.
*/
bool
AppleSPIDevice::_UsbInterfaceActive()
{
	uint64 usbStatus;
	if (_EvaluateInteger("UIST", &usbStatus) == B_OK && usbStatus != 0) {
		TRACE_ALWAYS("keyboard is on USB, leaving it to the USB driver\n");
		return true;
	}

	bool preferUSB = false;
	void* settings = load_driver_settings(DRIVER_NAME);
	if (settings != NULL) {
		preferUSB = get_driver_boolean_parameter(settings, "prefer_usb", false,
			true);
		unload_driver_settings(settings);
	}

	if (preferUSB && _CallMethod("UIEN", 1) == B_OK) {
		TRACE_ALWAYS("moved keyboard to USB as configured\n");
		return true;
	}
	if (preferUSB)
		TRACE_ALWAYS("this machine has no USB keyboard interface\n");

	return false;
}


/*!	Apple's firmware describes the SPI timing in device properties, returned
	by a _DSM, rather than only in _CRS. Use them where present.
*/
void
AppleSPIDevice::_ReadAppleProperties()
{
	acpi_object_type arguments[4];
	arguments[0].object_type = ACPI_TYPE_BUFFER;
	arguments[0].buffer.buffer = (void*)kApplePropertiesUUID;
	arguments[0].buffer.length = sizeof(kApplePropertiesUUID);
	arguments[1].object_type = ACPI_TYPE_INTEGER;
	arguments[1].integer.integer = 1;	// revision
	arguments[2].object_type = ACPI_TYPE_INTEGER;
	arguments[2].integer.integer = 1;	// function: get properties
	arguments[3].object_type = ACPI_TYPE_PACKAGE;
	arguments[3].package.count = 0;
	arguments[3].package.objects = NULL;

	acpi_objects args;
	args.count = 4;
	args.pointer = arguments;

	acpi_data buffer;
	buffer.pointer = NULL;
	buffer.length = ACPI_ALLOCATE_BUFFER;
	if (fACPI->evaluate_method(fHandle, "_DSM", &args, &buffer) != B_OK) {
		TRACE_ALWAYS("no device properties, using _CRS\n");
		return;
	}

	acpi_object_type* properties = (acpi_object_type*)buffer.pointer;
	if (properties == NULL || properties->object_type != ACPI_TYPE_PACKAGE) {
		free(buffer.pointer);
		return;
	}

	// The package holds name, value pairs.
	for (uint32 i = 0; i + 1 < properties->package.count; i += 2) {
		acpi_object_type* name = &properties->package.objects[i];
		acpi_object_type* value = &properties->package.objects[i + 1];
		if (name->object_type != ACPI_TYPE_STRING || name->string.string == NULL)
			continue;

		uint64 number = 0;
		if (value->object_type == ACPI_TYPE_INTEGER)
			number = value->integer.integer;
		else if (value->object_type == ACPI_TYPE_BUFFER) {
			const uint8* data = (const uint8*)value->buffer.buffer;
			for (uint32 j = 0; j < value->buffer.length && j < 8; j++)
				number |= (uint64)data[j] << (j * 8);
		} else
			continue;

		const char* key = name->string.string;
		if (strcmp(key, "spiSclkPeriod") == 0 && number > 0) {
			// in nanoseconds
			fConfig.speed_hz = (uint32)(1000000000ULL / number);
		} else if (strcmp(key, "spiWordSize") == 0 && number > 0
			&& number <= 32) {
			fConfig.bits_per_word = (uint8)number;
		} else if (strcmp(key, "spiSPO") == 0) {
			if (number != 0)
				fConfig.mode |= SPI_CPOL;
			else
				fConfig.mode &= ~SPI_CPOL;
		} else if (strcmp(key, "spiSPH") == 0) {
			if (number != 0)
				fConfig.mode |= SPI_CPHA;
			else
				fConfig.mode &= ~SPI_CPHA;
		} else if (strcmp(key, "spiCSDelay") == 0) {
			fCSDelay = (uint32)min_c(number, 1000);
		}
		// spiBitOrder is not used: the Intel controllers these machines
		// have only send the most significant bit first, so that is what
		// the device expects.
	}

	free(buffer.pointer);
}


status_t
AppleSPIDevice::_EvaluateInteger(const char* method, uint64* value)
{
	acpi_object_type object;
	acpi_data buffer;
	buffer.pointer = &object;
	buffer.length = sizeof(object);

	status_t status = fACPI->evaluate_method(fHandle, method, NULL, &buffer);
	if (status != B_OK)
		return status;
	if (object.object_type != ACPI_TYPE_INTEGER)
		return B_BAD_VALUE;

	*value = object.integer.integer;
	return B_OK;
}


status_t
AppleSPIDevice::_CallMethod(const char* method, uint64 argument)
{
	acpi_object_type object;
	object.object_type = ACPI_TYPE_INTEGER;
	object.integer.integer = argument;

	acpi_objects args;
	args.count = 1;
	args.pointer = &object;

	return fACPI->evaluate_method(fHandle, method, &args, NULL);
}


status_t
AppleSPIDevice::_InstallGPE()
{
	uint64 gpe;
	status_t status = _EvaluateInteger("_GPE", &gpe);
	if (status != B_OK)
		return status;

	status = fACPI->install_gpe_handler(NULL, (uint32)gpe,
		ACPI_GPE_LEVEL_TRIGGERED, &_GPEHandler, this);
	if (status != B_OK) {
		ERROR("installing GPE %" B_PRIu64 " handler failed: %s\n", gpe,
			strerror(status));
		return status;
	}

	status = fACPI->enable_gpe(NULL, (uint32)gpe);
	if (status != B_OK) {
		ERROR("enabling GPE %" B_PRIu64 " failed: %s\n", gpe,
			strerror(status));
		fACPI->remove_gpe_handler(NULL, (uint32)gpe, &_GPEHandler);
		return status;
	}

	fGPE = (int32)gpe;
	return B_OK;
}


void
AppleSPIDevice::_Stop()
{
	if (fGPE >= 0) {
		fACPI->disable_gpe(NULL, fGPE);
		fACPI->remove_gpe_handler(NULL, fGPE, &_GPEHandler);
		fGPE = -1;
	}

	if (fThread >= 0) {
		fStopping = true;
		release_sem(fEvent);
		status_t result;
		wait_for_thread(fThread, &result);
		fThread = -1;
	}

	if (fEvent >= 0) {
		delete_sem(fEvent);
		fEvent = -1;
	}
}


void
AppleSPIDevice::Removed()
{
	fRemoved = true;
	if (fKeyboard != NULL)
		fKeyboard->Removed();
	if (fMediaKeys != NULL)
		fMediaKeys->Removed();
	if (fTouchpad != NULL)
		fTouchpad->Removed();
	_Stop();
}


void
AppleSPIDevice::AcquireReference()
{
	atomic_add(&fReferences, 1);
}


//!	Returns whether the object may be deleted now.
bool
AppleSPIDevice::ReleaseReference()
{
	return atomic_add(&fReferences, -1) == 1 && fRemoved;
}


void
AppleSPIDevice::SetCapsLockLED(bool on)
{
	if (atomic_get_and_set(&fCapsLock, on ? 1 : 0) == (on ? 1 : 0))
		return;

	atomic_or(&fPendingCommands, kCommandCapsLock);
	if (fEvent >= 0)
		release_sem_etc(fEvent, 1, B_DO_NOT_RESCHEDULE);
}


//	#pragma mark - reader thread


/*static*/ uint32
AppleSPIDevice::_GPEHandler(acpi_handle gpeDevice, uint32 gpeNumber,
	void* context)
{
	// The GPE stays disabled until the thread has read the packet and
	// calls finish_gpe(); the line is level triggered and is only released
	// by reading.
	AppleSPIDevice* device = (AppleSPIDevice*)context;
	atomic_set(&device->fGPEFired, 1);
	release_sem_etc(device->fEvent, 1, B_DO_NOT_RESCHEDULE);
	return ACPI_INTERRUPT_HANDLED;
}


/*static*/ status_t
AppleSPIDevice::_ThreadEntry(void* data)
{
	((AppleSPIDevice*)data)->_Run();
	return B_OK;
}


void
AppleSPIDevice::_Run()
{
	while (!fStopping) {
		bigtime_t timeout = fGPE < 0 ? kPollInterval
			: fWriteInFlight ? kWriteTimeout / 4 : B_INFINITE_TIMEOUT;
		if (!fBacklightIdle && fBacklight > 0 && fBacklightTimeout > 0) {
			// wake up to turn the backlight off
			bigtime_t left = max_c(0,
				fLastActivity + fBacklightTimeout - system_time());
			if (timeout == B_INFINITE_TIMEOUT || left < timeout)
				timeout = left;
		}
		if (timeout == B_INFINITE_TIMEOUT)
			acquire_sem(fEvent);
		else
			acquire_sem_etc(fEvent, 1, B_RELATIVE_TIMEOUT, timeout);
		if (fStopping)
			break;

		bool fired = atomic_get_and_set(&fGPEFired, 0) != 0;
		if (fired || fGPE < 0) {
			status_t status = _ReadPacket();
			if (fired)
				fACPI->finish_gpe(NULL, fGPE);
			if (status == B_OK)
				_HandlePacket();
		}

		if (fWriteInFlight && system_time() - fWriteTime > kWriteTimeout) {
			ERROR("no reply to command, going on\n");
			fWriteInFlight = false;
		}

		_CheckBacklightIdle();

		if (!fWriteInFlight)
			_SendNextCommand();
	}
}


status_t
AppleSPIDevice::_ReadPacket()
{
	spi_transfer transfers[2] = {
		{ NULL, NULL, 0, fCSDelay, false },
		{ NULL, fReadBuffer, APPLE_SPI_PACKET_SIZE, 0, false },
	};

	status_t status = fSPI->acquire_bus(fSPICookie);
	if (status != B_OK)
		return status;
	status = fSPI->transfer(fSPICookie, transfers, 2);
	fSPI->release_bus(fSPICookie);

	if (status != B_OK && fBadPackets++ < 10)
		ERROR("read failed: %s\n", strerror(status));
	return status;
}


status_t
AppleSPIDevice::_WriteMessage(uint8 device, uint16 type,
	uint16 responseLength, const uint8* payload, size_t length)
{
	size_t messageLength = APPLE_SPI_MESSAGE_HEADER + length + 2;
	if (messageLength > APPLE_SPI_PACKET_DATA)
		return B_BAD_VALUE;

	uint8* packet = fWriteBuffer;
	memset(packet, 0, APPLE_SPI_PACKET_SIZE);
	packet[APPLE_SPI_PACKET_FLAGS] = APPLE_SPI_WRITE;
	packet[APPLE_SPI_PACKET_DEVICE] = device;
	apple_spi_put_le16(packet + APPLE_SPI_PACKET_LENGTH, messageLength);

	uint8* message = packet + APPLE_SPI_PACKET_HEADER;
	apple_spi_put_le16(message + APPLE_SPI_MESSAGE_TYPE, type);
	message[APPLE_SPI_MESSAGE_COUNTER] = fMessageCounter++;
	apple_spi_put_le16(message + APPLE_SPI_MESSAGE_RSP_LEN,
		responseLength != 0 ? responseLength : length);
	apple_spi_put_le16(message + APPLE_SPI_MESSAGE_LENGTH, length);
	if (length > 0)
		memcpy(message + APPLE_SPI_MESSAGE_HEADER, payload, length);
	apple_spi_put_le16(message + APPLE_SPI_MESSAGE_HEADER + length,
		apple_spi_crc16(message, APPLE_SPI_MESSAGE_HEADER + length));

	apple_spi_put_le16(packet + APPLE_SPI_PACKET_SIZE - 2,
		apple_spi_crc16(packet, APPLE_SPI_PACKET_SIZE - 2));

	// The device answers a write with a short status, after a pause with
	// chip select released; the reply proper comes later, with the GPE.
	spi_transfer transfers[3] = {
		{ NULL, NULL, 0, fCSDelay, false },
		{ packet, NULL, APPLE_SPI_PACKET_SIZE, APPLE_SPI_WRITE_STATUS_DELAY,
			true },
		{ NULL, fWriteStatus, APPLE_SPI_STATUS_SIZE, 0, false },
	};

	// A write sent right after reading a packet was answered with garbage
	// (b3 b3 b3 b3) on a MacBookPro14,1. Give the device a pause first,
	// with chip select released.
	snooze(APPLE_SPI_WRITE_STATUS_DELAY);

	status_t status = fSPI->acquire_bus(fSPICookie);
	if (status != B_OK)
		return status;
	status = fSPI->transfer(fSPICookie, transfers, 3);
	fSPI->release_bus(fSPICookie);
	if (status != B_OK) {
		ERROR("write failed: %s\n", strerror(status));
		return status;
	}

	fWriteInFlight = true;
	fWriteTime = system_time();

	if (memcmp(fWriteStatus, kAppleSPIStatusOK, sizeof(fWriteStatus)) != 0) {
		ERROR("command 0x%04x: unexpected status %02x %02x %02x %02x\n",
			type, fWriteStatus[0], fWriteStatus[1], fWriteStatus[2],
			fWriteStatus[3]);
		return B_BAD_DATA;
	}

	TRACE_ALWAYS("command 0x%04x sent\n", type);
	return B_OK;
}


//!	Sends one pending command, in order of importance. Returns whether any.
bool
AppleSPIDevice::_SendNextCommand()
{
	int32 pending = atomic_get(&fPendingCommands);
	if (pending == 0)
		return false;

	static const int32 kOrder[] = {
		kCommandTouchpadInfo, kCommandMultitouch, kCommandCapsLock,
		kCommandBacklight
	};
	int32 command = 0;
	for (size_t i = 0; i < B_COUNT_OF(kOrder); i++) {
		if ((pending & kOrder[i]) != 0) {
			command = kOrder[i];
			break;
		}
	}
	if (command == 0)
		return false;
	atomic_and(&fPendingCommands, ~command);

	// A command whose status reads wrong is sent again, a few times.
	status_t status = B_ERROR;
	for (int attempt = 0; attempt < kCommandAttempts; attempt++) {
		if (attempt > 0)
			snooze(kCommandRetryDelay);

		if (command == kCommandTouchpadInfo) {
			status = _WriteMessage(APPLE_SPI_DEVICE_INFO,
				APPLE_SPI_CMD_TP_INFO, 0x0200, NULL, 0);
		} else if (command == kCommandMultitouch) {
			uint8 payload[2];
			apple_spi_put_le16(payload, APPLE_SPI_MT_INIT_MAGIC);
			status = _WriteMessage(APPLE_SPI_DEVICE_TOUCHPAD,
				APPLE_SPI_CMD_MT_INIT, 0x0002, payload, sizeof(payload));
		} else if (command == kCommandCapsLock) {
			uint8 payload[2] = {
				0x01, (uint8)(atomic_get(&fCapsLock) != 0
					? APPLE_SPI_CAPS_LOCK_ON : 0)
			};
			status = _WriteMessage(APPLE_SPI_DEVICE_KEYBOARD,
				APPLE_SPI_CMD_CAPS_LOCK, 0, payload, sizeof(payload));
		} else {
			uint16 level = _BacklightLevel();
			uint8 payload[6];
			apple_spi_put_le16(payload, APPLE_SPI_BACKLIGHT_MAGIC);
			apple_spi_put_le16(payload + 2, level);
			apple_spi_put_le16(payload + 4, level != 0
				? APPLE_SPI_BACKLIGHT_ON : APPLE_SPI_BACKLIGHT_OFF);
			status = _WriteMessage(APPLE_SPI_DEVICE_KEYBOARD,
				APPLE_SPI_CMD_BACKLIGHT, 0, payload, sizeof(payload));
		}
		if (status != B_BAD_DATA)
			break;
	}

	// Even after a bad status, wait for a reply; the device may have taken
	// the command all the same.
	return status == B_OK || status == B_BAD_DATA;
}


void
AppleSPIDevice::_HandlePacket()
{
	const uint8* packet = fReadBuffer;

	// Also catches an idle bus read as all zeroes or all ones.
	uint8 flags = packet[APPLE_SPI_PACKET_FLAGS];
	if ((flags != APPLE_SPI_READ && flags != APPLE_SPI_WRITE)
		|| apple_spi_crc16(packet, APPLE_SPI_PACKET_SIZE) != 0) {
		if (fGPE >= 0 && fBadPackets++ < 10)
			ERROR("dropped bad packet, flags 0x%02x\n", flags);
		return;
	}

	uint8 device = packet[APPLE_SPI_PACKET_DEVICE];
	uint16 offset = apple_spi_le16(packet + APPLE_SPI_PACKET_OFFSET);
	uint16 remaining = apple_spi_le16(packet + APPLE_SPI_PACKET_REMAINING);
	uint16 length = apple_spi_le16(packet + APPLE_SPI_PACKET_LENGTH);

	if (length > APPLE_SPI_PACKET_DATA
		|| (size_t)offset + length > sizeof(fMessage)
		|| offset != (offset == 0 ? 0 : fMessageLength)) {
		TRACE("packet out of sequence: offset %u, length %u\n", offset,
			length);
		fMessageLength = 0;
		return;
	}

	memcpy(fMessage + offset, packet + APPLE_SPI_PACKET_HEADER, length);
	fMessageLength = offset + length;
	if (remaining != 0)
		return;

	size_t messageLength = fMessageLength;
	fMessageLength = 0;
	_HandleMessage(flags, device, fMessage, messageLength);
}


void
AppleSPIDevice::_HandleMessage(uint8 flags, uint8 device,
	const uint8* message, size_t length)
{
	if (length < APPLE_SPI_MESSAGE_HEADER + 2
		|| apple_spi_crc16(message, length) != 0
		|| apple_spi_le16(message + APPLE_SPI_MESSAGE_LENGTH)
			!= length - APPLE_SPI_MESSAGE_HEADER - 2) {
		if (fBadPackets++ < 10)
			ERROR("dropped bad message, %" B_PRIuSIZE " bytes\n", length);
		return;
	}

	uint16 type = apple_spi_le16(message + APPLE_SPI_MESSAGE_TYPE);
	const uint8* payload = message + APPLE_SPI_MESSAGE_HEADER;
	size_t payloadLength = length - APPLE_SPI_MESSAGE_HEADER - 2;

	if (flags == APPLE_SPI_WRITE) {
		// the reply to our last command
		fWriteInFlight = false;
		TRACE_ALWAYS("reply to command 0x%04x, device 0x%02x, %" B_PRIuSIZE
			" bytes\n", type, device, payloadLength);
		if (device == APPLE_SPI_DEVICE_INFO && type == APPLE_SPI_CMD_TP_INFO
			&& payloadLength > APPLE_SPI_TP_INFO_MODEL) {
			fTouchpad->SetModel(payload[APPLE_SPI_TP_INFO_MODEL]);
		}
		return;
	}

	if (device != APPLE_SPI_DEVICE_KEYBOARD
		&& fLoggedMessages < kLoggedMessages) {
		fLoggedMessages++;
		TRACE_ALWAYS("message 0x%04x from device 0x%02x, %" B_PRIuSIZE
			" bytes\n", type, device, payloadLength);
	}

	if (device == APPLE_SPI_DEVICE_KEYBOARD
		|| device == APPLE_SPI_DEVICE_TOUCHPAD) {
		_NoteActivity();
	}

	switch (device) {
		case APPLE_SPI_DEVICE_KEYBOARD:
			_HandleKeyboard(payload, payloadLength);
			break;
		case APPLE_SPI_DEVICE_TOUCHPAD:
			fTouchpad->HandleReport(payload, payloadLength);
			break;
		default:
			TRACE("message 0x%04x for unknown device 0x%02x\n", type, device);
			break;
	}
}


//!	Turns an Apple keyboard report into a HID boot keyboard report.
void
AppleSPIDevice::_HandleKeyboard(const uint8* payload, size_t length)
{
	if (length < APPLE_SPI_KBD_PAYLOAD)
		return;

	const uint8* keys = payload + APPLE_SPI_KBD_KEYS;
	bool fn = payload[APPLE_SPI_KBD_FN] != 0;

	uint8 report[APPLE_SPI_BOOT_REPORT_SIZE];
	memset(report, 0, sizeof(report));
	// The modifier bits are laid out as in HID.
	report[0] = payload[APPLE_SPI_KBD_MODIFIERS];

	uint8 backlightKeys = 0;
	uint8 mediaKey = 0;
	for (int i = 0; i < APPLE_SPI_KBD_KEY_COUNT; i++) {
		uint8 key = keys[i];
		if (key == HID_KEY_ERROR_ROLLOVER) {
			// Too many keys at once; the state is unknown.
			return;
		}
		if (!fn && (key == HID_KEY_F5 || key == HID_KEY_F6)) {
			backlightKeys |= key == HID_KEY_F5
				? kBacklightDownKey : kBacklightUpKey;
			continue;
		}
		if (!fn) {
			bool isMediaKey = false;
			for (size_t j = 0; j < B_COUNT_OF(kMediaKeys); j++) {
				if (kMediaKeys[j].key == key) {
					// One at a time; the last one pressed wins.
					mediaKey = kMediaKeys[j].usage;
					isMediaKey = true;
					break;
				}
			}
			if (isMediaKey)
				continue;
		}
		if (fn) {
			for (size_t j = 0; j < B_COUNT_OF(kFnKeys); j++) {
				if (kFnKeys[j].key == key) {
					key = kFnKeys[j].fnKey;
					break;
				}
			}
		}
		report[2 + i] = key;
	}

	// One step per press; holding the key does not repeat.
	uint8 pressed = backlightKeys & ~fBacklightKeys;
	fBacklightKeys = backlightKeys;
	if ((pressed & kBacklightDownKey) != 0)
		_SetBacklight(fBacklight - 1);
	if ((pressed & kBacklightUpKey) != 0)
		_SetBacklight(fBacklight + 1);

	fKeyboard->QueueReport(report);

	// The shared handler repeats a held key on its own, so only changes
	// need to go out.
	if (mediaKey != fMediaKey) {
		fMediaKey = mediaKey;
		fMediaKeys->QueueReport(&mediaKey);
	}
}


//!	Called by the thread; the command goes out from its loop.
void
AppleSPIDevice::_SetBacklight(int32 step)
{
	step = max_c(0, min_c(step, kBacklightSteps));
	if (step == fBacklight)
		return;

	fBacklight = step;
	atomic_or(&fPendingCommands, kCommandBacklight);
}


//!	The level to send: step 1 is the lowest that lights up, the last step
//	the brightest.
uint16
AppleSPIDevice::_BacklightLevel() const
{
	if (fBacklightIdle || fBacklight <= 0)
		return 0;

	return APPLE_SPI_BACKLIGHT_MIN
		+ (APPLE_SPI_BACKLIGHT_MAX - APPLE_SPI_BACKLIGHT_MIN)
			* (fBacklight - 1) / (kBacklightSteps - 1);
}


//!	A key press or a touch: turns the backlight back on if it went idle.
void
AppleSPIDevice::_NoteActivity()
{
	fLastActivity = system_time();
	if (!fBacklightIdle)
		return;

	fBacklightIdle = false;
	if (fBacklight > 0)
		atomic_or(&fPendingCommands, kCommandBacklight);
}


void
AppleSPIDevice::_CheckBacklightIdle()
{
	if (fBacklightIdle || fBacklight <= 0 || fBacklightTimeout <= 0
		|| system_time() - fLastActivity < fBacklightTimeout) {
		return;
	}

	fBacklightIdle = true;
	atomic_or(&fPendingCommands, kCommandBacklight);
}
