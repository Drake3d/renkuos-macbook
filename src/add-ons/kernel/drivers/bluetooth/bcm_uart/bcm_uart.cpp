/*
 * Copyright 2026, RenkuOS Contributors
 * All rights reserved. Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 */

/*
 * Broadcom Bluetooth on an Intel LPSS serial port (the BCM4350 of the
 * MacBookPro14,1: ACPI device BLTH, HID BCM2E7C, below the UART URT0,
 * PCI 8086:9d27).
 *
 * Phase 1, a probe: brings the serial port up, powers the Bluetooth part
 * of the chip on through Apple's ACPI methods, sends the standard HCI
 * commands Reset and Read Local Version Information over H4 (a packet
 * type byte, then the HCI packet) and logs the answers. It then powers
 * the chip off again; nothing is published yet.
 *
 * Serial port: a 16550 compatible (DesignWare) UART, registers 32 bits
 * apart, followed by the LPSS private registers at +0x200 (clock M/N
 * divider of the 120 MHz input, resets), as for our LPSS SPI driver.
 * The Broadcom chip starts at 115200 baud, 8N1, with RTS/CTS.
 *
 * Apple's ACPI methods below BLTH: BTPU and BTPD switch the Bluetooth
 * power on and off, BTLP(0) keeps the chip awake (1 lets it sleep).
 */


#include <ACPI.h>
#include <Drivers.h>
#include <KernelExport.h>
#include <bus/PCI.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <kernel.h>

extern "C" {
#	include "acpi.h"
}


#define BCM_UART_DRIVER_NAME	"drivers/bluetooth/bcm_uart/driver_v1"

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
static const uint32 kInitialBaud = 115200;

// Apple's device properties _DSM, a0b5b7c6-1318-441c-b0c9-fe695eaf949b.
static const uint8 kApplePropertiesUUID[16] = {
	0xc6, 0xb7, 0xb5, 0xa0, 0x18, 0x13, 0x1c, 0x44,
	0xb0, 0xc9, 0xfe, 0x69, 0x5e, 0xaf, 0x94, 0x9b
};

// H4 packet types
#define H4_COMMAND		0x01
#define H4_EVENT		0x04


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
};


static device_manager_info* sDeviceManager;
static acpi_module_info* sACPI;


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
	TRACE("clock register was 0x%08" B_PRIx32 "\n", info->savedClock);
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


static status_t
uart_read_byte(bcm_uart_info* info, uint8* _byte, bigtime_t deadline)
{
	while (true) {
		uint32 lsr = read_reg(info, UART_LSR);
		if ((lsr & LSR_ERRORS) != 0)
			TRACE("line status 0x%02" B_PRIx32 "\n", lsr & 0xff);
		if ((lsr & LSR_DATA_READY) != 0) {
			*_byte = read_reg(info, UART_RBR);
			return B_OK;
		}
		if (system_time() > deadline)
			return B_TIMED_OUT;
		snooze(50);
	}
}


//	#pragma mark - HCI


/*!	Sends HCI command \a opcode (no parameters) and waits up to one second
	for its Command Complete event; logs the event's bytes.
*/
static status_t
hci_command(bcm_uart_info* info, uint16 opcode, const char* name)
{
	uint8 packet[4] = { H4_COMMAND, (uint8)opcode, (uint8)(opcode >> 8), 0 };
	status_t status = uart_write(info, packet, sizeof(packet));
	if (status != B_OK)
		return status;

	bigtime_t deadline = system_time() + 1000000;
	uint8 event[260];
	for (int tries = 0; tries < 8; tries++) {
		// An event: type 0x04, event code, length, parameters.
		uint8 type;
		status = uart_read_byte(info, &type, deadline);
		if (status != B_OK) {
			TRACE("%s: no answer\n", name);
			return status;
		}
		if (type != H4_EVENT) {
			TRACE("%s: unexpected byte 0x%02x\n", name, type);
			continue;
		}
		uint8 code;
		uint8 length;
		if (uart_read_byte(info, &code, deadline) != B_OK
			|| uart_read_byte(info, &length, deadline) != B_OK) {
			TRACE("%s: event cut short\n", name);
			return B_TIMED_OUT;
		}
		for (uint32 i = 0; i < length; i++) {
			if (uart_read_byte(info, &event[i], deadline) != B_OK) {
				TRACE("%s: event cut short\n", name);
				return B_TIMED_OUT;
			}
		}

		char hex[3 * 64 + 1];
		size_t offset = 0;
		for (uint32 i = 0; i < length && i < 64; i++)
			offset += snprintf(hex + offset, sizeof(hex) - offset, " %02x",
				event[i]);
		hex[offset] = '\0';
		TRACE("%s: event 0x%02x, %u bytes:%s\n", name, code, length, hex);

		// Command Complete (0x0e) for this opcode ends the wait.
		if (code == 0x0e && length >= 4 && event[1] == (uint8)opcode
			&& event[2] == (uint8)(opcode >> 8)) {
			if (event[3] != 0) {
				TRACE("%s: status 0x%02x\n", name, event[3]);
				return B_ERROR;
			}
			if (opcode == 0x0c14 && length > 4) {
				char localName[249];
				uint32 nameLength = min_c((uint32)length - 4,
					(uint32)sizeof(localName) - 1);
				memcpy(localName, event + 4, nameLength);
				localName[nameLength] = '\0';
				TRACE("local name \"%s\"\n", localName);
			}
			if (opcode == 0x1009 && length >= 10) {
				TRACE("BD_ADDR %02x:%02x:%02x:%02x:%02x:%02x\n", event[9],
					event[8], event[7], event[6], event[5], event[4]);
			}
			if (opcode == 0x1001 && length >= 12) {
				TRACE("HCI version %u, revision 0x%04x, LMP version %u, "
					"manufacturer %u, LMP subversion 0x%04x\n", event[4],
					event[5] | (event[6] << 8), event[7],
					event[8] | (event[9] << 8), event[10] | (event[11] << 8));
			}
			return B_OK;
		}
	}
	return B_ERROR;
}


static void
log_status(bcm_uart_info* info, acpi_handle handle, const char* name)
{
	acpi_object_type value;
	if (sACPI->evaluate_object(handle, "_STA", NULL, &value, sizeof(value))
			== B_OK && value.object_type == ACPI_TYPE_INTEGER) {
		TRACE("%s _STA 0x%" B_PRIx64 "\n", name, value.integer.integer);
	} else
		TRACE("%s has no _STA\n", name);
}


//!	Logs Apple's device properties of the Bluetooth node.
static void
log_apple_properties(bcm_uart_info* info)
{
	acpi_object_type arguments[4];
	arguments[0].object_type = ACPI_TYPE_BUFFER;
	arguments[0].buffer.buffer = (void*)kApplePropertiesUUID;
	arguments[0].buffer.length = sizeof(kApplePropertiesUUID);
	arguments[1].object_type = ACPI_TYPE_INTEGER;
	arguments[1].integer.integer = 1;
	arguments[2].object_type = ACPI_TYPE_INTEGER;
	arguments[2].integer.integer = 1;
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
			!= B_OK) {
		TRACE("BLTH: no device properties\n");
		return;
	}
	acpi_object_type* properties = (acpi_object_type*)buffer.pointer;
	if (properties == NULL || properties->object_type != ACPI_TYPE_PACKAGE) {
		TRACE("BLTH: device properties are not a package\n");
		free(buffer.pointer);
		return;
	}
	for (uint32 i = 0; i + 1 < properties->package.count; i += 2) {
		acpi_object_type* name = &properties->package.objects[i];
		acpi_object_type* value = &properties->package.objects[i + 1];
		if (name->object_type != ACPI_TYPE_STRING
			|| name->string.string == NULL)
			continue;
		uint64 number = 0;
		if (value->object_type == ACPI_TYPE_INTEGER)
			number = value->integer.integer;
		else if (value->object_type == ACPI_TYPE_BUFFER) {
			const uint8* data = (const uint8*)value->buffer.buffer;
			for (uint32 j = 0; j < value->buffer.length && j < 8; j++)
				number |= (uint64)data[j] << (j * 8);
		}
		if (strcmp(name->string.string, "baud") == 0 && number > 0
			&& number <= 4000000)
			info->appleBaud = (uint32)number;

		if (value->object_type == ACPI_TYPE_INTEGER) {
			TRACE("BLTH property %s = 0x%" B_PRIx64 "\n", name->string.string,
				value->integer.integer);
		} else if (value->object_type == ACPI_TYPE_BUFFER) {
			char hex[3 * 16 + 1];
			size_t offset = 0;
			const uint8* data = (const uint8*)value->buffer.buffer;
			for (uint32 j = 0; j < value->buffer.length && j < 16; j++) {
				offset += snprintf(hex + offset, sizeof(hex) - offset, " %02x",
					data[j]);
			}
			hex[offset] = '\0';
			TRACE("BLTH property %s =%s\n", name->string.string, hex);
		} else if (value->object_type == ACPI_TYPE_STRING) {
			TRACE("BLTH property %s = \"%s\"\n", name->string.string,
				value->string.string);
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


static void
probe_chip(bcm_uart_info* info)
{
	log_status(info, info->bluetooth, "BLTH");
	log_apple_properties(info);
	// Apple's firmware leaves the chip at the speed of its "baud" property
	// (3 Mbaud on the MacBookPro14,1), not at Broadcom's usual 115200.
	uint32 baud = info->appleBaud != 0 ? info->appleBaud : 3000000;

	uart_reset(info);
	uart_set_baud(info, baud, true);

	// Power cycle the Bluetooth part and keep it awake.
	call_method(info, "BTPD");
	snooze(50000);
	call_method(info, "BTPU");
	call_method_with(info, "BTLP", 0);
	if (wait_for_cts(info, 1000000)) {
		uart_drain(info);
		if (hci_command(info, 0x0c03, "HCI Reset") == B_OK) {
			hci_command(info, 0x1001, "Read Local Version");
			hci_command(info, 0x1009, "Read BD_ADDR");
			hci_command(info, 0x0c14, "Read Local Name");
			// Broadcom: Read Verbose Config Version Info (chip and build).
			hci_command(info, 0xfc79, "Broadcom Verbose Config");
		}
	}

	// Phase 1 ends here: switch the Bluetooth part off again and give the
	// port its clock back.
	call_method_with(info, "BTLP", 1);
	call_method(info, "BTPD");
	write_reg(info, LPSS_CLOCK_PARAMS, info->savedClock | LPSS_CLOCK_UPDATE);
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
	TRACE("Bluetooth on serial port %04x at 0x%" B_PRIxPHYSADDR "\n",
		info->pciInfo.device_id, base);

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

	probe_chip(info);

	*_cookie = info;
	return B_OK;
}


static void
bcm_uart_uninit_driver(void* cookie)
{
	bcm_uart_info* info = (bcm_uart_info*)cookie;
	delete_area(info->registersArea);
	free(info);
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
	NULL,	// register child devices
	NULL,	// rescan
	NULL,	// removed
};


module_info* modules[] = {
	(module_info*)&sBcmUartDriver,
	NULL
};
