/*
 * Copyright 2026, RenkuOS Contributors
 * All rights reserved. Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 */

/*
 * Apple System Management Controller (SMC) sensors: the temperatures and
 * fan speeds of Intel Macs, read only. The SMC is the ACPI device
 * APP0001 and answers on two I/O ports (data at the base of its _CRS
 * range, normally 0x300, commands and status at base + 4). Every value
 * has a four-character key: "#KEY" counts the keys, "T..." keys are
 * temperatures, "FNum" counts the fans, "F<n>Ac", "F<n>Mn" and "F<n>Mx"
 * are a fan's current, minimum and maximum speed.
 *
 * Protocol, as publicly documented for these machines (written here from
 * that description, no code taken): a read sends a command byte (0x10
 * read, 0x12 key by index, 0x13 key type) to the command port, then the
 * key (or a 32-bit big-endian index) and the wanted length to the data
 * port, one byte at a time, then reads the answer from the data port.
 * Status bits on the command port: 0x01 a byte is waiting, 0x02 the SMC
 * is not taking input yet, 0x04 a command is in progress.
 *
 * The SMC keeps controlling the fans; this driver never writes values.
 * Readings: "cat /dev/sensor/apple_smc/0", and once in the syslog at boot.
 */


#include <ACPI.h>
#include <Drivers.h>
#include <Errors.h>
#include <ISA.h>
#include <KernelExport.h>
#include <lock.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <kernel.h>
#include <util/AutoLock.h>

extern "C" {
#	include "acpi.h"
}


#define APPLE_SMC_DRIVER_NAME	"drivers/sensor/apple_smc/driver_v1"
#define APPLE_SMC_DEVICE_NAME	"drivers/sensor/apple_smc/device_v1"
#define APPLE_SMC_BASENAME		"sensor/apple_smc/%d"
#define APPLE_SMC_PATHID		"apple_smc/path_id"
#define APPLE_SMC_HID			"APP0001"

#define TRACE(x...)				dprintf("apple_smc: " x)
#define ERROR(x...)				dprintf("apple_smc: " x)


static const uint16 kDefaultPortBase = 0x300;
static const uint16 kCommandOffset = 4;

static const uint8 kCommandRead = 0x10;
static const uint8 kCommandKeyByIndex = 0x12;
static const uint8 kCommandKeyType = 0x13;

static const uint8 kStatusDataWaiting = 0x01;
static const uint8 kStatusInputClosed = 0x02;
static const uint8 kStatusBusy = 0x04;

static const int32 kMaxTemperatures = 64;
static const int32 kMaxFans = 4;
static const size_t kReportSize = 4096;


// What some temperature keys measure, on the MacBook Pro of 2016-2017;
// other keys are listed by key only.
static const struct {
	const char*	key;
	const char*	name;
} kTemperatureNames[] = {
	{ "TB0T", "battery" },
	{ "TB1T", "battery 1" },
	{ "TB2T", "battery 2" },
	{ "TC0E", "CPU die (virtual)" },
	{ "TC0F", "CPU die (filtered)" },
	{ "TC0P", "CPU proximity" },
	{ "TC1C", "CPU core 1" },
	{ "TC2C", "CPU core 2" },
	{ "TCGC", "integrated GPU" },
	{ "TCSA", "CPU system agent" },
	{ "TCXC", "CPU PECI" },
	{ "TH0A", "SSD A" },
	{ "TH0B", "SSD B" },
	{ "TM0P", "memory proximity" },
	{ "TPCD", "platform controller hub die" },
	{ "TW0P", "Wi-Fi proximity" },
	{ "Ta0P", "ambient" },
	{ "Th1H", "heat pipe" },
	{ "Ts0P", "palm rest" },
	{ "Ts0S", "memory bank proximity" },
	{ "Ts1P", "palm rest 2" },
};


struct smc_driver_cookie {
	device_node*				node;
	acpi_device_module_info*	acpi;
	acpi_device					acpi_cookie;

	uint16						dataPort;
	uint16						commandPort;
	mutex						lock;

	uint32						keyCount;
	int32						fanCount;
	int32						temperatureCount;
	char						temperatures[kMaxTemperatures][5];
};


static device_manager_info* sDeviceManager;
static isa_module_info* sISA;


//	#pragma mark - SMC access


static status_t
wait_status(smc_driver_cookie* smc, uint8 value, uint8 mask)
{
	// Most answers come within microseconds; allow up to about 50 ms.
	for (int32 i = 0; i < 200; i++) {
		if ((sISA->read_io_8(smc->commandPort) & mask) == value)
			return B_OK;
		if (i < 20)
			spin(10);
		else
			snooze(250);
	}
	return B_TIMED_OUT;
}


static status_t
send_command(smc_driver_cookie* smc, uint8 command)
{
	status_t status = wait_status(smc, 0, kStatusInputClosed);
	if (status != B_OK)
		return status;
	sISA->write_io_8(smc->commandPort, command);
	return B_OK;
}


static status_t
send_data(smc_driver_cookie* smc, uint8 value)
{
	// The SMC is ready for the next byte once it takes input again and
	// still works on the command.
	status_t status = wait_status(smc, 0, kStatusInputClosed);
	if (status == B_OK)
		status = wait_status(smc, kStatusBusy, kStatusBusy);
	if (status != B_OK)
		return status;
	sISA->write_io_8(smc->dataPort, value);
	return B_OK;
}


//!	Gets the SMC out of a command another user left unfinished.
static status_t
make_idle(smc_driver_cookie* smc)
{
	if (wait_status(smc, 0, kStatusBusy) == B_OK)
		return B_OK;
	status_t status = send_command(smc, kCommandRead);
	if (status != B_OK)
		return status;
	return wait_status(smc, 0, kStatusBusy);
}


/*!	Runs one command: \a command, then the four bytes of \a argument (a
	key, or a big-endian index), then the answer's length; reads \a length
	bytes into \a buffer. The caller holds the lock.
*/
static status_t
smc_transfer(smc_driver_cookie* smc, uint8 command, const uint8* argument,
	uint8* buffer, uint8 length)
{
	status_t status = make_idle(smc);
	if (status == B_OK)
		status = send_command(smc, command);
	for (int i = 0; status == B_OK && i < 4; i++)
		status = send_data(smc, argument[i]);
	if (status == B_OK)
		status = send_data(smc, length);
	for (uint8 i = 0; status == B_OK && i < length; i++) {
		status = wait_status(smc, kStatusDataWaiting | kStatusBusy,
			kStatusDataWaiting | kStatusBusy);
		if (status == B_OK)
			buffer[i] = sISA->read_io_8(smc->dataPort);
	}

	// Take any bytes the SMC still offers, then let it finish.
	for (int i = 0; i < 16; i++) {
		spin(10);
		if ((sISA->read_io_8(smc->commandPort) & kStatusDataWaiting) == 0)
			break;
		sISA->read_io_8(smc->dataPort);
	}
	status_t idle = wait_status(smc, 0, kStatusBusy);
	return status != B_OK ? status : idle;
}


static status_t
smc_read_key(smc_driver_cookie* smc, const char* key, uint8* buffer,
	uint8 length)
{
	return smc_transfer(smc, kCommandRead, (const uint8*)key, buffer, length);
}


static status_t
smc_key_at(smc_driver_cookie* smc, uint32 index, char* key)
{
	uint8 argument[4] = {
		(uint8)(index >> 24), (uint8)(index >> 16), (uint8)(index >> 8),
		(uint8)index
	};
	status_t status = smc_transfer(smc, kCommandKeyByIndex, argument,
		(uint8*)key, 4);
	key[4] = '\0';
	return status;
}


//!	The key's data size and four-character type (such as "sp78").
static status_t
smc_key_type(smc_driver_cookie* smc, const char* key, uint8* _size,
	char* type)
{
	uint8 info[6];
	status_t status = smc_transfer(smc, kCommandKeyType, (const uint8*)key,
		info, sizeof(info));
	if (status != B_OK)
		return status;
	*_size = info[0];
	memcpy(type, info + 1, 4);
	type[4] = '\0';
	return B_OK;
}


//	#pragma mark - values


//!	A temperature ("sp78": signed 8.8 fixed point) in tenths of a degree.
static status_t
read_temperature(smc_driver_cookie* smc, const char* key, int32* _tenths)
{
	uint8 data[2];
	status_t status = smc_read_key(smc, key, data, sizeof(data));
	if (status != B_OK)
		return status;
	int32 raw = (int16)((data[0] << 8) | data[1]);
	*_tenths = raw * 10 / 256;
	return B_OK;
}


/*!	A fan speed in rpm: "fpe2" (unsigned 14.2 fixed point, big endian) on
	these Macs, "flt " (a 32-bit float, little endian) on later ones;
	converted without floating point, which the kernel does not use.
*/
static status_t
read_fan_speed(smc_driver_cookie* smc, const char* key, int32* _rpm)
{
	uint8 size;
	char type[5];
	status_t status = smc_key_type(smc, key, &size, type);
	if (status != B_OK)
		return status;

	uint8 data[4];
	if (strcmp(type, "fpe2") == 0 && size == 2) {
		status = smc_read_key(smc, key, data, 2);
		if (status == B_OK)
			*_rpm = ((data[0] << 8) | data[1]) >> 2;
		return status;
	}
	if (strcmp(type, "flt ") == 0 && size == 4) {
		status = smc_read_key(smc, key, data, 4);
		if (status != B_OK)
			return status;
		uint32 bits = data[0] | (data[1] << 8) | (data[2] << 16)
			| ((uint32)data[3] << 24);
		int32 exponent = (int32)((bits >> 23) & 0xff) - 127;
		uint32 mantissa = (bits & 0x7fffff) | 0x800000;
		if ((bits & 0x80000000) != 0 || exponent < 0)
			*_rpm = 0;
		else if (exponent > 23)
			*_rpm = 0x7fffffff;
		else
			*_rpm = mantissa >> (23 - exponent);
		return B_OK;
	}
	return B_NOT_SUPPORTED;
}


static const char*
temperature_name(const char* key)
{
	for (size_t i = 0; i < B_COUNT_OF(kTemperatureNames); i++) {
		if (strcmp(kTemperatureNames[i].key, key) == 0)
			return kTemperatureNames[i].name;
	}
	return NULL;
}


//!	Writes the current readings as text; the caller holds the lock.
static size_t
write_report(smc_driver_cookie* smc, char* report, size_t size)
{
	size_t length = 0;
#define APPEND(x...) \
	do { \
		if (length < size) \
			length += snprintf(report + length, size - length, x); \
	} while (false)

	for (int32 fan = 0; fan < smc->fanCount; fan++) {
		char key[5];
		int32 current = -1;
		int32 minimum = -1;
		int32 maximum = -1;
		snprintf(key, sizeof(key), "F%" B_PRId32 "Ac", fan);
		read_fan_speed(smc, key, &current);
		snprintf(key, sizeof(key), "F%" B_PRId32 "Mn", fan);
		read_fan_speed(smc, key, &minimum);
		snprintf(key, sizeof(key), "F%" B_PRId32 "Mx", fan);
		read_fan_speed(smc, key, &maximum);
		APPEND("fan %" B_PRId32 ": %" B_PRId32 " rpm (minimum %" B_PRId32
			", maximum %" B_PRId32 ")\n", fan, current, minimum, maximum);
	}

	for (int32 i = 0; i < smc->temperatureCount; i++) {
		const char* key = smc->temperatures[i];
		int32 tenths;
		if (read_temperature(smc, key, &tenths) != B_OK)
			continue;
		// Missing sensors read as 0 or far below zero.
		if (tenths <= 0)
			continue;
		const char* name = temperature_name(key);
		APPEND("%s %3" B_PRId32 ".%" B_PRId32 " C%s%s\n", key, tenths / 10,
			tenths % 10, name != NULL ? "  " : "", name != NULL ? name : "");
	}
#undef APPEND
	return min_c(length, size - 1);
}


//	#pragma mark - device module API


struct smc_device_cookie {
	smc_driver_cookie*	smc;
	char*				report;
	size_t				reportLength;
};


static status_t
apple_smc_init_device(void* driverCookie, void** cookie)
{
	*cookie = driverCookie;
	return B_OK;
}


static void
apple_smc_uninit_device(void* cookie)
{
}


static status_t
apple_smc_open(void* initCookie, const char* path, int flags, void** _cookie)
{
	smc_device_cookie* cookie
		= (smc_device_cookie*)calloc(1, sizeof(smc_device_cookie));
	if (cookie == NULL)
		return B_NO_MEMORY;
	cookie->smc = (smc_driver_cookie*)initCookie;
	*_cookie = cookie;
	return B_OK;
}


static status_t
apple_smc_close(void* cookie)
{
	return B_OK;
}


static status_t
apple_smc_free(void* _cookie)
{
	smc_device_cookie* cookie = (smc_device_cookie*)_cookie;
	free(cookie->report);
	free(cookie);
	return B_OK;
}


static status_t
apple_smc_read(void* _cookie, off_t position, void* buffer, size_t* _length)
{
	smc_device_cookie* cookie = (smc_device_cookie*)_cookie;
	if (position < 0)
		return B_BAD_VALUE;

	// Readings are taken when reading starts and kept for this open file,
	// so a reader that reads in pieces gets one consistent report.
	if (position == 0 || cookie->report == NULL) {
		if (cookie->report == NULL) {
			cookie->report = (char*)malloc(kReportSize);
			if (cookie->report == NULL)
				return B_NO_MEMORY;
		}
		MutexLocker locker(cookie->smc->lock);
		cookie->reportLength = write_report(cookie->smc, cookie->report,
			kReportSize);
	}

	if ((size_t)position >= cookie->reportLength) {
		*_length = 0;
		return B_OK;
	}
	size_t length = min_c(*_length, cookie->reportLength - (size_t)position);
	if (user_memcpy(buffer, cookie->report + position, length) != B_OK)
		return B_BAD_ADDRESS;
	*_length = length;
	return B_OK;
}


static status_t
apple_smc_write(void* cookie, off_t position, const void* buffer,
	size_t* _length)
{
	return B_NOT_ALLOWED;
}


static status_t
apple_smc_control(void* cookie, uint32 op, void* arg, size_t length)
{
	return B_DEV_INVALID_IOCTL;
}


//	#pragma mark - driver module API


static float
apple_smc_support(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK || strcmp(bus, "acpi") != 0)
		return 0.0;

	uint32 type;
	if (sDeviceManager->get_attr_uint32(parent, ACPI_DEVICE_TYPE_ITEM, &type,
			false) != B_OK || type != ACPI_TYPE_DEVICE)
		return 0.0;

	const char* hid;
	if (sDeviceManager->get_attr_string(parent, ACPI_DEVICE_HID_ITEM, &hid,
			false) != B_OK || strcmp(hid, APPLE_SMC_HID) != 0)
		return 0.0;

	return 0.6;
}


static status_t
apple_smc_register_device(device_node* node)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "Apple SMC sensors" }},
		{ NULL }
	};
	return sDeviceManager->register_node(node, APPLE_SMC_DRIVER_NAME, attrs,
		NULL, NULL);
}


static acpi_status
find_io_range(acpi_resource* resource, void* context)
{
	uint16* base = (uint16*)context;
	if (resource->Type == ACPI_RESOURCE_TYPE_IO && *base == 0
		&& resource->Data.Io.AddressLength >= kCommandOffset + 1) {
		*base = resource->Data.Io.Minimum;
	}
	return AE_OK;
}


static status_t
apple_smc_init_driver(device_node* node, void** driverCookie)
{
	smc_driver_cookie* smc
		= (smc_driver_cookie*)calloc(1, sizeof(smc_driver_cookie));
	if (smc == NULL)
		return B_NO_MEMORY;
	smc->node = node;
	mutex_init(&smc->lock, "apple smc");

	device_node* parent = sDeviceManager->get_parent_node(node);
	sDeviceManager->get_driver(parent, (driver_module_info**)&smc->acpi,
		(void**)&smc->acpi_cookie);
	sDeviceManager->put_node(parent);

	uint16 base = 0;
	smc->acpi->walk_resources(smc->acpi_cookie, (char*)"_CRS", find_io_range,
		&base);
	if (base == 0)
		base = kDefaultPortBase;
	smc->dataPort = base;
	smc->commandPort = base + kCommandOffset;

	MutexLocker locker(smc->lock);

	uint8 data[4];
	if (smc_read_key(smc, "#KEY", data, 4) != B_OK) {
		ERROR("no answer from the SMC at 0x%x\n", base);
		locker.Unlock();
		mutex_destroy(&smc->lock);
		free(smc);
		return B_ERROR;
	}
	smc->keyCount = ((uint32)data[0] << 24) | (data[1] << 16) | (data[2] << 8)
		| data[3];

	if (smc_read_key(smc, "FNum", data, 1) == B_OK)
		smc->fanCount = min_c((int32)data[0], kMaxFans);

	// Find the temperature keys once; reading them is then cheap.
	for (uint32 i = 0; i < smc->keyCount && i < 4096
			&& smc->temperatureCount < kMaxTemperatures; i++) {
		char key[5];
		if (smc_key_at(smc, i, key) != B_OK)
			break;
		if (key[0] != 'T')
			continue;
		uint8 size;
		char type[5];
		if (smc_key_type(smc, key, &size, type) != B_OK
			|| strcmp(type, "sp78") != 0 || size != 2)
			continue;
		strlcpy(smc->temperatures[smc->temperatureCount++], key, 5);
	}

	TRACE("SMC at 0x%x: %" B_PRIu32 " keys, %" B_PRId32 " temperature "
		"sensors, %" B_PRId32 " fans\n", base, smc->keyCount,
		smc->temperatureCount, smc->fanCount);

	// The first readings, for the syslog.
	char* report = (char*)malloc(kReportSize);
	if (report != NULL) {
		size_t length = write_report(smc, report, kReportSize);
		report[length] = '\0';
		char* line = report;
		while (*line != '\0') {
			char* end = strchr(line, '\n');
			if (end != NULL)
				*end = '\0';
			TRACE("  %s\n", line);
			if (end == NULL)
				break;
			line = end + 1;
		}
		free(report);
	}

	*driverCookie = smc;
	return B_OK;
}


static void
apple_smc_uninit_driver(void* driverCookie)
{
	smc_driver_cookie* smc = (smc_driver_cookie*)driverCookie;
	mutex_destroy(&smc->lock);
	free(smc);
}


static status_t
apple_smc_register_child_devices(void* cookie)
{
	smc_driver_cookie* smc = (smc_driver_cookie*)cookie;
	int pathID = sDeviceManager->create_id(APPLE_SMC_PATHID);
	if (pathID < 0)
		return B_ERROR;

	char name[B_DEV_NAME_LENGTH];
	snprintf(name, sizeof(name), APPLE_SMC_BASENAME, pathID);
	return sDeviceManager->publish_device(smc->node, name,
		APPLE_SMC_DEVICE_NAME);
}


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{ B_ISA_MODULE_NAME, (module_info**)&sISA },
	{}
};


static driver_module_info sAppleSMCDriverModule = {
	{
		APPLE_SMC_DRIVER_NAME,
		0,
		NULL
	},
	apple_smc_support,
	apple_smc_register_device,
	apple_smc_init_driver,
	apple_smc_uninit_driver,
	apple_smc_register_child_devices,
	NULL,	// rescan
	NULL,	// removed
};


static struct device_module_info sAppleSMCDeviceModule = {
	{
		APPLE_SMC_DEVICE_NAME,
		0,
		NULL
	},
	apple_smc_init_device,
	apple_smc_uninit_device,
	NULL,	// removed
	apple_smc_open,
	apple_smc_close,
	apple_smc_free,
	apple_smc_read,
	apple_smc_write,
	NULL,	// io
	apple_smc_control,
	NULL,	// select
	NULL	// deselect
};


module_info* modules[] = {
	(module_info*)&sAppleSMCDriverModule,
	(module_info*)&sAppleSMCDeviceModule,
	NULL
};
