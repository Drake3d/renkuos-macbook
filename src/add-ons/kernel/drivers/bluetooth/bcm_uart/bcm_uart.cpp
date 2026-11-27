/*
 * Copyright 2026, RenkuOS Contributors
 * All rights reserved. Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 */

/*
 * Broadcom Bluetooth on an Intel LPSS serial port (the BCM4350 of the
 * MacBookPro14,1: ACPI device BLTH, HID BCM2E7C, below the UART URT0,
 * PCI 8086:9d27), as an H4 transport for the Bluetooth stack.
 *
 * The driver publishes bluetooth/h4/bcm_uart/0, which the Bluetooth server
 * finds (it expects a folder per driver below bluetooth/h4) and opens.
 * On open it powers the Bluetooth part of the chip on through Apple's
 * ACPI methods (BTPD/BTPU switch the power, BTLP(0) keeps the chip awake,
 * BTLP(1) lets it sleep), sets the serial port to the speed of Apple's
 * "baud" device property (3 Mbaud on the MacBookPro14,1; the chip starts
 * there, not at Broadcom's usual 115200), waits for the chip to raise CTS
 * and registers with the HCI layer. A thread reads the serial port and
 * hands every complete H4 packet (type byte, then the HCI packet) to the
 * HCI layer; RTS/CTS flow control holds the chip while the FIFO is full.
 * Commands and ACL data from the stack are written out with their H4
 * type byte. On close the chip is switched off again.
 *
 * Serial port: a 16550 compatible (DesignWare) UART, registers 32 bits
 * apart, followed by the LPSS private registers at +0x200 (clock M/N
 * divider of the 120 MHz input, resets), as for our LPSS SPI driver.
 *
 * The chip runs from its ROM firmware; no patch is loaded yet.
 *
 * "trace_packets true" in the driver settings file bcm_uart logs every
 * command's opcode and the start of every packet received or sent.
 */


#include <ACPI.h>
#include <Drivers.h>
#include <driver_settings.h>
#include <KernelExport.h>
#include <bus/PCI.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <kernel.h>
#include <lock.h>
#include <net_buffer.h>
#include <util/AutoLock.h>

#include <bluetooth/HCI/btHCI_transport.h>

extern "C" {
#	include "acpi.h"
}


#define BCM_UART_DRIVER_NAME	"drivers/bluetooth/bcm_uart/driver_v1"
#define BCM_UART_DEVICE_NAME	"drivers/bluetooth/bcm_uart/device_v1"
#define BCM_UART_DEVFS_NAME		"bluetooth/h4/bcm_uart/%d"
#define BCM_UART_PATHID			"bcm_uart/path_id"

#define TRACE(x...)				dprintf("bcm_uart: " x)


// LPSS serial ports that may carry the Bluetooth chip.
static const uint16 kUartDevices[] = {
	0x9d27, 0x9d28, 0x9d66,	// Sunrise Point-LP UART 0, 1, 2
	0xa127, 0xa128, 0xa166,	// Sunrise Point-H
	0
};

// 16550 registers, 32 bits apart
#define UART_RBR		0x00	// receive (read)
#define UART_THR		0x00	// transmit (write)
#define UART_DLL		0x00	// divisor low (DLAB)
#define UART_DLM		0x04	// divisor high (DLAB)
#define UART_IER		0x04
#define UART_FCR		0x08	// FIFO control (write)
#define UART_LCR		0x0c
#define UART_MCR		0x10
#define UART_LSR		0x14
#define UART_MSR		0x18

#define LCR_8N1			0x03
#define LCR_DLAB		0x80
#define FCR_ENABLE		0x01
#define FCR_CLEAR		0x06	// clear both FIFOs
#define MCR_DTR			0x01
#define MCR_RTS			0x02
#define MCR_AUTO_FLOW	0x20	// hardware RTS/CTS
#define LSR_DATA_READY	0x01
#define LSR_ERRORS		0x1e	// overrun, parity, framing, break
#define LSR_THR_EMPTY	0x20
#define MSR_CTS			0x10

// LPSS private registers
#define LPSS_CLOCK_PARAMS	0x200
#define LPSS_CLOCK_EN		(1u << 0)
#define LPSS_CLOCK_M_SHIFT	1
#define LPSS_CLOCK_N_SHIFT	16
#define LPSS_CLOCK_UPDATE	(1u << 31)
#define LPSS_RESETS			0x204
#define LPSS_RESETS_FUNC	(1u << 2)
#define LPSS_RESETS_IDMA	0x3

// The LPSS input clock; the M/N divider makes 16 times the baud rate, so
// the 16550 divisor stays 1 (120 MHz * 48 / 3125 = 16 * 115200).
static const uint64 kInputClock = 120000000;

// Apple's device properties _DSM, a0b5b7c6-1318-441c-b0c9-fe695eaf949b.
static const uint8 kApplePropertiesUUID[16] = {
	0xc6, 0xb7, 0xb5, 0xa0, 0x18, 0x13, 0x1c, 0x44,
	0xb0, 0xc9, 0xfe, 0x69, 0x5e, 0xaf, 0x94, 0x9b
};

// H4 packet types
#define H4_COMMAND		0x01
#define H4_ACL			0x02
#define H4_SCO			0x03
#define H4_EVENT		0x04

static const size_t kMaxPacket = HCI_MAX_FRAME_SIZE;
static const uint32 kDefaultBaud = 3000000;


struct bcm_uart_info {
	device_node*			node;
	pci_device_module_info*	pci;
	pci_device*				device;
	pci_info				pciInfo;

	area_id					registersArea;
	volatile uint8*			registers;

	acpi_handle				port;
	acpi_handle				bluetooth;
	uint32					appleBaud;		// Apple's "baud" property, or 0
	uint32					savedClock;		// LPSS clock as the firmware left it

	mutex					lock;			// open and close
	mutex					writeLock;		// one packet at a time
	bool					isOpen;
	thread_id				readerThread;
	volatile bool			running;
	hci_id					hciID;
	bt_hci_statistics		statistics;

	// The H4 packet being received.
	uint8					rxType;			// 0: waiting for a type byte
	uint32					rxHeaderSize;
	uint32					rxExpected;
	uint32					rxCount;
	uint32					rxDiscarded;
	uint8					rxPacket[HCI_MAX_FRAME_SIZE];
};


static device_manager_info* sDeviceManager;
static acpi_module_info* sACPI;
static bt_hci_module_info* sHCI;
static net_buffer_module_info* sBufferModule;

// There is one such port; the HCI hooks only get the HCI id.
static bcm_uart_info* sDevice;

static bool sTracePackets;


static void
trace_packet(const char* direction, uint8 type, const uint8* data,
	size_t length)
{
	char hex[3 * 16 + 1];
	size_t offset = 0;
	for (size_t i = 0; i < length && i < 16; i++)
		offset += snprintf(hex + offset, sizeof(hex) - offset, " %02x", data[i]);
	hex[offset] = '\0';
	TRACE("%s %s, %" B_PRIuSIZE " bytes:%s\n", direction,
		type == H4_COMMAND ? "command" : type == H4_EVENT ? "event"
			: type == H4_ACL ? "ACL" : "SCO", length, hex);
}


static inline uint32
read_reg(bcm_uart_info* info, uint32 offset)
{
	return *(volatile uint32*)(info->registers + offset);
}


static inline void
write_reg(bcm_uart_info* info, uint32 offset, uint32 value)
{
	*(volatile uint32*)(info->registers + offset) = value;
}


//	#pragma mark - ACPI


//!	Finds the ACPI node of the PCI function, by its _ADR.
static acpi_handle
find_acpi_handle(const pci_info& pciInfo)
{
	uint32 address = ((uint32)pciInfo.device << 16) | pciInfo.function;
	device_attr attrs[] = {
		{ B_DEVICE_BUS, B_STRING_TYPE, { .string = "acpi" }},
		{ ACPI_DEVICE_ADDR_ITEM, B_UINT32_TYPE, { .ui32 = address }},
		{ NULL }
	};

	device_node* root = sDeviceManager->get_root_node();
	device_node* acpiNode = NULL;
	status_t status = sDeviceManager->find_child_node(root, attrs, &acpiNode);
	sDeviceManager->put_node(root);
	if (status != B_OK)
		return NULL;

	acpi_handle handle = NULL;
	const char* path;
	if (sDeviceManager->get_attr_string(acpiNode, ACPI_DEVICE_PATH_ITEM,
			&path, false) == B_OK) {
		sACPI->get_handle(NULL, path, &handle);
	}
	sDeviceManager->put_node(acpiNode);
	return handle;
}


static status_t
call_method(bcm_uart_info* info, const char* method)
{
	status_t status = sACPI->evaluate_method(info->bluetooth, method, NULL,
		NULL);
	if (status != B_OK)
		TRACE("%s failed: %s\n", method, strerror(status));
	return status;
}


static status_t
call_method_with(bcm_uart_info* info, const char* method, uint64 value)
{
	acpi_object_type argument;
	argument.object_type = ACPI_TYPE_INTEGER;
	argument.integer.integer = value;
	acpi_objects arguments;
	arguments.count = 1;
	arguments.pointer = &argument;
	status_t status = sACPI->evaluate_method(info->bluetooth, method,
		&arguments, NULL);
	if (status != B_OK)
		TRACE("%s(%" B_PRIu64 ") failed: %s\n", method, value,
			strerror(status));
	return status;
}


//	#pragma mark - serial port


static uint32
gcd(uint32 a, uint32 b)
{
	while (b != 0) {
		uint32 t = a % b;
		a = b;
		b = t;
	}
	return a;
}


//!	Sets the LPSS clock to 16 times \a baud and the 16550 divisor to 1.
static void
uart_set_baud(bcm_uart_info* info, uint32 baud, bool flowControl)
{
	uint32 m = baud * 16;
	uint32 n = (uint32)kInputClock;
	uint32 divisor = gcd(m, n);
	m /= divisor;
	n /= divisor;
	while (m > 0x7fff || n > 0x7fff) {
		m = (m + 1) / 2;
		n = (n + 1) / 2;
	}
	uint32 clock = LPSS_CLOCK_EN | (m << LPSS_CLOCK_M_SHIFT)
		| (n << LPSS_CLOCK_N_SHIFT);
	write_reg(info, LPSS_CLOCK_PARAMS, clock);
	write_reg(info, LPSS_CLOCK_PARAMS, clock | LPSS_CLOCK_UPDATE);

	write_reg(info, UART_IER, 0);
	write_reg(info, UART_LCR, LCR_DLAB | LCR_8N1);
	write_reg(info, UART_DLL, 1);
	write_reg(info, UART_DLM, 0);
	write_reg(info, UART_LCR, LCR_8N1);
	write_reg(info, UART_FCR, FCR_ENABLE | FCR_CLEAR);
	write_reg(info, UART_MCR, MCR_DTR | MCR_RTS
		| (flowControl ? MCR_AUTO_FLOW : 0));
	TRACE("serial port at %" B_PRIu32 " baud (M/N %" B_PRIu32 "/%" B_PRIu32
		")%s: LSR 0x%02" B_PRIx32 ", MSR 0x%02" B_PRIx32 "\n", baud, m, n,
		flowControl ? " with RTS/CTS" : "", read_reg(info, UART_LSR) & 0xff,
		read_reg(info, UART_MSR) & 0xff);
}


static void
uart_reset(bcm_uart_info* info)
{
	// Function and DMA out of reset.
	info->savedClock = read_reg(info, LPSS_CLOCK_PARAMS);
	write_reg(info, LPSS_RESETS, 0);
	snooze(10);
	write_reg(info, LPSS_RESETS, LPSS_RESETS_FUNC | LPSS_RESETS_IDMA);
}


static void
uart_drain(bcm_uart_info* info)
{
	for (int i = 0; i < 4096
			&& (read_reg(info, UART_LSR) & LSR_DATA_READY) != 0; i++) {
		read_reg(info, UART_RBR);
	}
}


static status_t
uart_write(bcm_uart_info* info, const uint8* data, size_t length)
{
	for (size_t i = 0; i < length; i++) {
		bigtime_t deadline = system_time() + 100000;
		while ((read_reg(info, UART_LSR) & LSR_THR_EMPTY) == 0) {
			if (system_time() > deadline) {
				TRACE("write timed out at byte %" B_PRIuSIZE " (MSR 0x%02"
					B_PRIx32 ", CTS %s)\n", i, read_reg(info, UART_MSR) & 0xff,
					(read_reg(info, UART_MSR) & MSR_CTS) != 0 ? "on" : "off");
				return B_TIMED_OUT;
			}
			spin(10);
		}
		write_reg(info, UART_THR, data[i]);
	}
	return B_OK;
}


//	#pragma mark - power and setup


//!	Reads the speed of the link from Apple's device properties (_DSM).
static void
read_apple_properties(bcm_uart_info* info)
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
	if (sACPI->evaluate_method(info->bluetooth, "_DSM", &args, &buffer)
			!= B_OK)
		return;

	// The package holds name, value pairs; "baud" is an 8-byte buffer.
	acpi_object_type* properties = (acpi_object_type*)buffer.pointer;
	if (properties != NULL && properties->object_type == ACPI_TYPE_PACKAGE) {
		for (uint32 i = 0; i + 1 < properties->package.count; i += 2) {
			acpi_object_type* name = &properties->package.objects[i];
			acpi_object_type* value = &properties->package.objects[i + 1];
			if (name->object_type != ACPI_TYPE_STRING
				|| name->string.string == NULL
				|| strcmp(name->string.string, "baud") != 0)
				continue;
			uint64 number = 0;
			if (value->object_type == ACPI_TYPE_INTEGER)
				number = value->integer.integer;
			else if (value->object_type == ACPI_TYPE_BUFFER) {
				const uint8* data = (const uint8*)value->buffer.buffer;
				for (uint32 j = 0; j < value->buffer.length && j < 8; j++)
					number |= (uint64)data[j] << (j * 8);
			}
			if (number > 0 && number <= 4000000)
				info->appleBaud = (uint32)number;
		}
	}
	free(buffer.pointer);
}


//!	Waits up to \a timeout for the chip to raise CTS (ready to receive).
static bool
wait_for_cts(bcm_uart_info* info, bigtime_t timeout)
{
	bigtime_t start = system_time();
	while (system_time() - start < timeout) {
		if ((read_reg(info, UART_MSR) & MSR_CTS) != 0) {
			TRACE("chip ready (CTS) after %" B_PRId64 " ms\n",
				(system_time() - start) / 1000);
			return true;
		}
		snooze(2000);
	}
	TRACE("chip not ready (no CTS) after %" B_PRId64 " ms\n", timeout / 1000);
	return false;
}




/*!	Sends HCI Reset and waits up to a second for its Command Complete
	(04 0e 04 01 03 0c, then the status), before the reader thread runs.
*/
static status_t
reset_chip(bcm_uart_info* info)
{
	static const uint8 kReset[] = { H4_COMMAND, 0x03, 0x0c, 0x00 };
	status_t status = uart_write(info, kReset, sizeof(kReset));
	if (status != B_OK)
		return status;

	static const uint8 kComplete[] = { H4_EVENT, 0x0e, 0x04, 0x01, 0x03,
		0x0c };
	uint8 answer[sizeof(kComplete) + 1];
	uint32 count = 0;
	bigtime_t deadline = system_time() + 1000000;
	while (count < sizeof(answer)) {
		if ((read_reg(info, UART_LSR) & LSR_DATA_READY) == 0) {
			if (system_time() > deadline) {
				TRACE("no answer to HCI Reset\n");
				return B_TIMED_OUT;
			}
			snooze(100);
			continue;
		}
		uint8 byte = read_reg(info, UART_RBR);
		// Skip anything before the event starts.
		if (count == 0 && byte != H4_EVENT)
			continue;
		answer[count++] = byte;
	}
	if (memcmp(answer, kComplete, sizeof(kComplete)) != 0
		|| answer[sizeof(kComplete)] != 0) {
		TRACE("unexpected answer to HCI Reset: %02x %02x %02x %02x %02x %02x "
			"%02x\n", answer[0], answer[1], answer[2], answer[3], answer[4],
			answer[5], answer[6]);
		return B_ERROR;
	}
	return B_OK;
}


static void
power_off(bcm_uart_info* info)
{
	call_method_with(info, "BTLP", 1);
	call_method(info, "BTPD");
	write_reg(info, LPSS_CLOCK_PARAMS, info->savedClock | LPSS_CLOCK_UPDATE);
}


//!	Powers the Bluetooth part on and waits until it takes data.
static status_t
power_on(bcm_uart_info* info)
{
	uint32 baud = info->appleBaud != 0 ? info->appleBaud : kDefaultBaud;
	uart_reset(info);
	uart_set_baud(info, baud, true);

	call_method(info, "BTPD");
	snooze(50000);
	if (call_method(info, "BTPU") != B_OK)
		return B_ERROR;
	call_method_with(info, "BTLP", 0);

	if (!wait_for_cts(info, 1000000)) {
		power_off(info);
		return B_TIMED_OUT;
	}
	uart_drain(info);

	// Fresh from its ROM the chip refuses configuration commands ("Command
	// Disallowed") until it has been reset, and the Bluetooth server does
	// not send a reset: send one here, before the stack sees the chip.
	status_t status = reset_chip(info);
	if (status != B_OK) {
		power_off(info);
		return status;
	}
	return B_OK;
}


//	#pragma mark - receiving


//!	Hands a complete H4 packet to the HCI layer.
static void
deliver_packet(bcm_uart_info* info)
{
	if (sTracePackets)
		trace_packet("rx", info->rxType, info->rxPacket, info->rxCount);

	bt_packet_t type;
	switch (info->rxType) {
		case H4_EVENT:
			type = BT_EVENT;
			info->statistics.eventRX++;
			break;
		case H4_ACL:
			type = BT_ACL;
			info->statistics.aclRX++;
			break;
		default:
			type = BT_SCO;
			info->statistics.scoRX++;
			break;
	}
	info->statistics.bytesRX += info->rxCount;

	if (sHCI->PostTransportPacket(info->hciID, type, info->rxPacket,
			info->rxCount) == B_OK)
		info->statistics.successfulRX++;
	else
		info->statistics.errorRX++;
}


//!	Feeds one received byte into the H4 packet being assembled.
static void
receive_byte(bcm_uart_info* info, uint8 byte)
{
	if (info->rxType == 0) {
		// A packet starts with its type; anything else is noise.
		switch (byte) {
			case H4_EVENT:
				info->rxHeaderSize = 2;		// event code, length
				break;
			case H4_ACL:
				info->rxHeaderSize = 4;		// handle, 16-bit length
				break;
			case H4_SCO:
				info->rxHeaderSize = 3;		// handle, length
				break;
			default:
				if (info->rxDiscarded++ == 0)
					TRACE("unexpected byte 0x%02x, skipping\n", byte);
				return;
		}
		info->rxType = byte;
		info->rxCount = 0;
		info->rxExpected = info->rxHeaderSize;
		return;
	}

	info->rxPacket[info->rxCount++] = byte;
	if (info->rxCount == info->rxHeaderSize) {
		uint32 payload;
		if (info->rxType == H4_EVENT)
			payload = info->rxPacket[1];
		else if (info->rxType == H4_ACL)
			payload = info->rxPacket[2] | (info->rxPacket[3] << 8);
		else
			payload = info->rxPacket[2];
		info->rxExpected = info->rxHeaderSize + payload;
		if (info->rxExpected > sizeof(info->rxPacket)) {
			TRACE("packet of %" B_PRIu32 " bytes is too big, dropped\n",
				info->rxExpected);
			info->statistics.rejectedRX++;
			info->rxType = 0;
			return;
		}
	}
	if (info->rxCount >= info->rxHeaderSize
		&& info->rxCount == info->rxExpected) {
		deliver_packet(info);
		info->rxType = 0;
	}
}


/*!	Reads the serial port. The FIFO holds 64 bytes; when it is full the
	UART drops RTS and the chip waits, so polling loses nothing.
*/
static status_t
reader_thread(void* data)
{
	bcm_uart_info* info = (bcm_uart_info*)data;
	while (info->running) {
		bool received = false;
		for (int i = 0; i < 512; i++) {
			uint32 lsr = read_reg(info, UART_LSR);
			if ((lsr & LSR_ERRORS) != 0)
				info->statistics.errorRX++;
			if ((lsr & LSR_DATA_READY) == 0)
				break;
			receive_byte(info, read_reg(info, UART_RBR));
			received = true;
		}
		if (!received)
			snooze(1000);
	}
	return B_OK;
}


//	#pragma mark - sending


static status_t
send_packet(bcm_uart_info* info, uint8 type, const uint8* data, size_t length)
{

	if (sTracePackets)
		trace_packet("tx", type, data, length);

	MutexLocker locker(info->writeLock);
	status_t status = uart_write(info, &type, 1);
	if (status == B_OK)
		status = uart_write(info, data, length);
	if (status == B_OK) {
		info->statistics.successfulTX++;
		info->statistics.bytesTX += length + 1;
	} else
		info->statistics.errorTX++;
	return status;
}


static status_t
send_command_hook(hci_id id, net_buffer* buffer)
{
	bcm_uart_info* info = sDevice;
	if (info == NULL || !info->isOpen || id != info->hciID)
		return B_ERROR;
	if (buffer->size == 0 || buffer->size > HCI_MAX_EVENT_SIZE)
		return B_BAD_VALUE;

	// The buffer stays the caller's, as with the USB transport.
	uint8 command[HCI_MAX_EVENT_SIZE];
	size_t length = buffer->size;
	status_t status = sBufferModule->read(buffer, 0, command, length);
	if (status != B_OK)
		return status;
	info->statistics.commandTX++;
	return send_packet(info, H4_COMMAND, command, length);
}


static status_t
send_acl_hook(hci_id id, net_buffer* buffer)
{
	bcm_uart_info* info = sDevice;
	if (info == NULL || !info->isOpen || id != info->hciID)
		return B_ERROR;
	if (buffer->size == 0 || buffer->size > HCI_MAX_FRAME_SIZE)
		return B_BAD_VALUE;

	uint8* data = (uint8*)malloc(buffer->size);
	if (data == NULL)
		return B_NO_MEMORY;
	size_t length = buffer->size;
	status_t status = sBufferModule->read(buffer, 0, data, length);
	if (status == B_OK) {
		info->statistics.aclTX++;
		status = send_packet(info, H4_ACL, data, length);
	}
	free(data);

	// Sent ACL buffers are freed by the transport, as with USB.
	if (status == B_OK)
		sBufferModule->free(buffer);
	return status;
}


static status_t
send_sco_hook(hci_id id, net_buffer* buffer)
{
	return B_NOT_SUPPORTED;
}


static status_t
deliver_statistics_hook(hci_id id, bt_hci_statistics* statistics)
{
	bcm_uart_info* info = sDevice;
	if (info == NULL || id != info->hciID)
		return B_ERROR;
	memcpy(statistics, &info->statistics, sizeof(bt_hci_statistics));
	return B_OK;
}


static bt_hci_transport_hooks sHooks = {
	send_command_hook,
	send_acl_hook,
	send_sco_hook,
	NULL,	// eSCO
	deliver_statistics_hook,
	H4
};


//	#pragma mark - device module


static status_t
bcm_uart_init_device(void* driverCookie, void** _cookie)
{
	*_cookie = driverCookie;
	return B_OK;
}


static void
bcm_uart_uninit_device(void* cookie)
{
}


static status_t
bcm_uart_open(void* initCookie, const char* path, int flags, void** _cookie)
{
	bcm_uart_info* info = (bcm_uart_info*)initCookie;
	MutexLocker locker(info->lock);
	if (info->isOpen)
		return B_BUSY;

	if (get_module(BT_HCI_MODULE_NAME, (module_info**)&sHCI) != B_OK) {
		TRACE("no Bluetooth stack (%s)\n", BT_HCI_MODULE_NAME);
		return B_ERROR;
	}
	if (get_module(NET_BUFFER_MODULE_NAME, (module_info**)&sBufferModule)
			!= B_OK) {
		put_module(BT_HCI_MODULE_NAME);
		return B_ERROR;
	}

	status_t status = power_on(info);
	if (status != B_OK) {
		TRACE("the Bluetooth chip does not come up: %s\n", strerror(status));
		put_module(NET_BUFFER_MODULE_NAME);
		put_module(BT_HCI_MODULE_NAME);
		return status;
	}

	bluetooth_device* device;
	status = sHCI->RegisterDriver(&sHooks, &device);
	if (status != B_OK) {
		power_off(info);
		put_module(NET_BUFFER_MODULE_NAME);
		put_module(BT_HCI_MODULE_NAME);
		return status;
	}
	info->hciID = device->index;
	memset(&info->statistics, 0, sizeof(info->statistics));
	info->rxType = 0;
	info->rxDiscarded = 0;
	info->isOpen = true;

	info->running = true;
	info->readerThread = spawn_kernel_thread(reader_thread, "bcm_uart reader",
		B_REAL_TIME_DISPLAY_PRIORITY, info);
	if (info->readerThread >= 0)
		resume_thread(info->readerThread);

	TRACE("Bluetooth controller up as HCI device %" B_PRId32 "\n",
		info->hciID);
	*_cookie = info;
	return B_OK;
}


static status_t
bcm_uart_close(void* cookie)
{
	bcm_uart_info* info = (bcm_uart_info*)cookie;
	MutexLocker locker(info->lock);
	if (!info->isOpen)
		return B_OK;

	info->running = false;
	if (info->readerThread >= 0) {
		status_t result;
		wait_for_thread(info->readerThread, &result);
		info->readerThread = -1;
	}
	sHCI->UnregisterDriver(info->hciID);
	info->isOpen = false;
	power_off(info);

	put_module(NET_BUFFER_MODULE_NAME);
	put_module(BT_HCI_MODULE_NAME);
	TRACE("Bluetooth controller down\n");
	return B_OK;
}


static status_t
bcm_uart_free(void* cookie)
{
	return B_OK;
}


static status_t
bcm_uart_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	*_length = 0;
	return B_OK;
}


static status_t
bcm_uart_write(void* cookie, off_t position, const void* buffer,
	size_t* _length)
{
	return B_NOT_ALLOWED;
}


static status_t
copy_out(void* to, const void* from, size_t length)
{
	if (IS_USER_ADDRESS(to))
		return user_memcpy(to, from, length);
	memcpy(to, from, length);
	return B_OK;
}


static status_t
bcm_uart_control(void* cookie, uint32 op, void* argument, size_t length)
{
	bcm_uart_info* info = (bcm_uart_info*)cookie;
	if (argument == NULL)
		return B_BAD_VALUE;

	switch (op) {
		case GET_HCI_ID:
			return copy_out(argument, &info->hciID, sizeof(hci_id));

		case GET_STATS:
			return copy_out(argument, &info->statistics,
				sizeof(bt_hci_statistics));

		case BT_UP:
			// Reading runs from the open on.
			return B_OK;

		case ISSUE_BT_COMMAND:
		{
			// An HCI command from the Bluetooth server, without type byte.
			if (length == 0 || length > HCI_MAX_EVENT_SIZE)
				return B_BAD_VALUE;
			uint8 command[HCI_MAX_EVENT_SIZE];
			if (IS_USER_ADDRESS(argument)) {
				if (user_memcpy(command, argument, length) != B_OK)
					return B_BAD_ADDRESS;
			} else
				memcpy(command, argument, length);
			info->statistics.commandTX++;
			return send_packet(info, H4_COMMAND, command, length);
		}
	}
	return B_DEV_INVALID_IOCTL;
}


//	#pragma mark - driver module


static float
bcm_uart_supports(device_node* parent)
{
	const char* bus;
	uint16 vendor;
	uint16 device;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK
		|| strcmp(bus, "pci") != 0
		|| sDeviceManager->get_attr_uint16(parent, B_DEVICE_VENDOR_ID,
			&vendor, false) != B_OK
		|| sDeviceManager->get_attr_uint16(parent, B_DEVICE_ID, &device,
			false) != B_OK
		|| vendor != 0x8086)
		return 0.0;

	for (int i = 0; kUartDevices[i] != 0; i++) {
		if (kUartDevices[i] == device)
			return 0.6;
	}
	return 0.0;
}


static status_t
bcm_uart_register_device(device_node* parent)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "Broadcom Bluetooth (UART)" }},
		{ NULL }
	};
	return sDeviceManager->register_node(parent, BCM_UART_DRIVER_NAME, attrs,
		NULL, NULL);
}


static status_t
bcm_uart_init_driver(device_node* node, void** _cookie)
{
	bcm_uart_info* info = (bcm_uart_info*)calloc(1, sizeof(bcm_uart_info));
	if (info == NULL)
		return B_NO_MEMORY;
	info->node = node;
	info->registersArea = -1;
	info->readerThread = -1;

	device_node* parent = sDeviceManager->get_parent_node(node);
	sDeviceManager->get_driver(parent, (driver_module_info**)&info->pci,
		(void**)&info->device);
	sDeviceManager->put_node(parent);
	info->pci->get_pci_info(info->device, &info->pciInfo);

	// Only the port the Bluetooth chip hangs off: its ACPI node has BLTH.
	info->port = find_acpi_handle(info->pciInfo);
	if (info->port == NULL
		|| sACPI->get_handle(info->port, "BLTH", &info->bluetooth) != B_OK) {
		free(info);
		return B_ERROR;
	}

	phys_addr_t base = info->pciInfo.u.h0.base_registers[0];
	size_t size = info->pciInfo.u.h0.base_register_sizes[0];
	if ((info->pciInfo.u.h0.base_register_flags[0] & PCI_address_type)
			== PCI_address_type_64) {
		base |= (uint64)info->pciInfo.u.h0.base_registers[1] << 32;
	}
	if (base == 0 || size < LPSS_RESETS + 4) {
		TRACE("BAR 0 not assigned\n");
		free(info);
		return B_ERROR;
	}

	info->pci->set_powerstate(info->device, PCI_pm_state_d0);
	uint16 command = info->pci->read_pci_config(info->device, PCI_command, 2);
	info->pci->write_pci_config(info->device, PCI_command, 2,
		command | PCI_command_memory);

	info->registersArea = map_physical_memory("bcm_uart registers", base,
		size, B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		(void**)&info->registers);
	if (info->registersArea < 0) {
		status_t status = info->registersArea;
		free(info);
		return status;
	}

	read_apple_properties(info);

	void* settings = load_driver_settings("bcm_uart");
	if (settings != NULL) {
		sTracePackets = get_driver_boolean_parameter(settings,
			"trace_packets", false, true);
		unload_driver_settings(settings);
	}

	mutex_init(&info->lock, "bcm_uart");
	mutex_init(&info->writeLock, "bcm_uart write");
	sDevice = info;

	TRACE("Bluetooth on serial port %04x at 0x%" B_PRIxPHYSADDR ", %" B_PRIu32
		" baud%s\n", info->pciInfo.device_id, base,
		info->appleBaud != 0 ? info->appleBaud : kDefaultBaud,
		sTracePackets ? ", tracing packets" : "");
	*_cookie = info;
	return B_OK;
}


static void
bcm_uart_uninit_driver(void* cookie)
{
	bcm_uart_info* info = (bcm_uart_info*)cookie;
	if (sDevice == info)
		sDevice = NULL;
	mutex_destroy(&info->lock);
	mutex_destroy(&info->writeLock);
	delete_area(info->registersArea);
	free(info);
}


static status_t
bcm_uart_register_child_devices(void* cookie)
{
	bcm_uart_info* info = (bcm_uart_info*)cookie;
	int pathID = sDeviceManager->create_id(BCM_UART_PATHID);
	if (pathID < 0)
		return B_ERROR;

	char name[B_DEV_NAME_LENGTH];
	snprintf(name, sizeof(name), BCM_UART_DEVFS_NAME, pathID);
	return sDeviceManager->publish_device(info->node, name,
		BCM_UART_DEVICE_NAME);
}


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{ B_ACPI_MODULE_NAME, (module_info**)&sACPI },
	{}
};


static driver_module_info sBcmUartDriver = {
	{
		BCM_UART_DRIVER_NAME,
		0,
		NULL
	},
	bcm_uart_supports,
	bcm_uart_register_device,
	bcm_uart_init_driver,
	bcm_uart_uninit_driver,
	bcm_uart_register_child_devices,
	NULL,	// rescan
	NULL,	// removed
};


static struct device_module_info sBcmUartDevice = {
	{
		BCM_UART_DEVICE_NAME,
		0,
		NULL
	},
	bcm_uart_init_device,
	bcm_uart_uninit_device,
	NULL,	// removed
	bcm_uart_open,
	bcm_uart_close,
	bcm_uart_free,
	bcm_uart_read,
	bcm_uart_write,
	NULL,	// io
	bcm_uart_control,
	NULL,	// select
	NULL	// deselect
};


module_info* modules[] = {
	(module_info*)&sBcmUartDriver,
	(module_info*)&sBcmUartDevice,
	NULL
};
