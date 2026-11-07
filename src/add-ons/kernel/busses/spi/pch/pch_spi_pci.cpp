/*
 * Copyright 2026, RenkuOS Contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ACPI.h>
#include <bus/PCI.h>

#include "pch_spi.h"


// Generic SPI ports of the Intel LPSS. Device IDs from the platform
// controller hub datasheets.
static const uint16 kPchSpiDevices[] = {
	0x9d29, 0x9d2a,		// Sunrise Point-LP (Skylake-U, Kaby Lake-U)
	0xa129, 0xa12a,		// Sunrise Point-H
	0xa2a9, 0xa2aa,		// Union Point-H (Kaby Lake-H)
	0
};


static bool
is_supported(uint16 deviceID)
{
	for (int i = 0; kPchSpiDevices[i] != 0; i++) {
		if (kPchSpiDevices[i] == deviceID)
			return true;
	}
	return false;
}


//!	Finds the ACPI node describing the PCI function, by its _ADR.
static acpi_handle
find_acpi_handle(const pci_info& info)
{
	uint32 addr = ((uint32)info.device << 16) | info.function;
	if (addr == 0)
		return NULL;

	device_attr acpiAttrs[] = {
		{ B_DEVICE_BUS, B_STRING_TYPE, { .string = "acpi" }},
		{ ACPI_DEVICE_ADDR_ITEM, B_UINT32_TYPE, { .ui32 = addr }},
		{ NULL }
	};

	device_node* rootNode = gDeviceManager->get_root_node();
	device_node* acpiNode = NULL;
	status_t status = gDeviceManager->find_child_node(rootNode, acpiAttrs,
		&acpiNode);
	gDeviceManager->put_node(rootNode);
	if (status != B_OK)
		return NULL;

	acpi_handle handle = NULL;
	const char* path;
	if (gDeviceManager->get_attr_string(acpiNode, ACPI_DEVICE_PATH_ITEM, &path,
			false) == B_OK) {
		gACPI->get_handle(NULL, path, &handle);
	}
	gDeviceManager->put_node(acpiNode);
	return handle;
}


//	#pragma mark -


static status_t
register_child_devices(void* cookie)
{
	CALLED();
	pch_spi_info* bus = (pch_spi_info*)cookie;

	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "PCH SPI Controller" }},
		{ B_DEVICE_FIXED_CHILD, B_STRING_TYPE,
			{ .string = SPI_FOR_CONTROLLER_MODULE_NAME }},
		{ NULL }
	};

	return gDeviceManager->register_node(bus->driver_node,
		PCH_SPI_SIM_MODULE_NAME, attrs, NULL, NULL);
}


static status_t
init_device(device_node* node, void** deviceCookie)
{
	CALLED();

	pch_spi_info* bus = (pch_spi_info*)calloc(1, sizeof(pch_spi_info));
	if (bus == NULL)
		return B_NO_MEMORY;

	{
		device_node* pciParent = gDeviceManager->get_parent_node(node);
		gDeviceManager->get_driver(pciParent, (driver_module_info**)&bus->pci,
			(void**)&bus->device);
		gDeviceManager->put_node(pciParent);
	}

	bus->driver_node = node;
	bus->registers_area = -1;

	pci_info* pciInfo = &bus->pciinfo;
	bus->pci->get_pci_info(bus->device, pciInfo);

	bus->base_addr = pciInfo->u.h0.base_registers[0];
	bus->map_size = pciInfo->u.h0.base_register_sizes[0];
	if ((pciInfo->u.h0.base_register_flags[0] & PCI_address_type)
			== PCI_address_type_64) {
		bus->base_addr |= (uint64)pciInfo->u.h0.base_registers[1] << 32;
		bus->map_size |= (uint64)pciInfo->u.h0.base_register_sizes[1] << 32;
	}
	if (bus->base_addr == 0 || bus->map_size < PCH_LPSS_CS_CONTROL + 4) {
		ERROR("PCI BAR not assigned\n");
		free(bus);
		return B_ERROR;
	}

	bus->pci->set_powerstate(bus->device, PCI_pm_state_d0);

	uint16 command = bus->pci->read_pci_config(bus->device, PCI_command, 2);
	command |= PCI_command_memory;
	bus->pci->write_pci_config(bus->device, PCI_command, 2, command);

	bus->acpiHandle = find_acpi_handle(*pciInfo);
	if (bus->acpiHandle == NULL)
		TRACE_ALWAYS("%02x:%02x.%x has no ACPI node\n", pciInfo->bus,
			pciInfo->device, pciInfo->function);

	*deviceCookie = bus;
	return B_OK;
}


static void
uninit_device(void* deviceCookie)
{
	free(deviceCookie);
}


static status_t
register_device(device_node* parent)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE, { .string = "PCH SPI PCI" }},
		{}
	};

	return gDeviceManager->register_node(parent,
		PCH_SPI_PCI_DEVICE_MODULE_NAME, attrs, NULL, NULL);
}


static float
supports_device(device_node* parent)
{
	CALLED();
	const char* bus;
	uint16 vendorID, deviceID;

	if (gDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			< B_OK
		|| gDeviceManager->get_attr_uint16(parent, B_DEVICE_VENDOR_ID,
			&vendorID, false) < B_OK
		|| gDeviceManager->get_attr_uint16(parent, B_DEVICE_ID, &deviceID,
			false) < B_OK) {
		return -1;
	}

	if (strcmp(bus, "pci") != 0 || vendorID != 0x8086
		|| !is_supported(deviceID)) {
		return 0.0f;
	}

	TRACE("found LPSS SPI port 0x%04x\n", deviceID);
	return 0.8f;
}


driver_module_info gPchSpiPciDevice = {
	{
		PCH_SPI_PCI_DEVICE_MODULE_NAME,
		0,
		NULL
	},

	supports_device,
	register_device,
	init_device,
	uninit_device,
	register_child_devices,
	NULL,	// rescan
	NULL,	// device removed
};
