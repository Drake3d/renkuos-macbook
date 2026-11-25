/*
 * Copyright 2026, RenkuOS Contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


/*!	Starts the camera's image processor (ISP): loads Apple's firmware into
	the camera's memory, powers the sensor, starts the processor, and sets
	up the message rings ("channels") the firmware and the host talk over.

	Sources, as noted at each step:
	[F]	The patjak/facetimehd Linux driver (fthd_isp.c, fthd_ringbuf.c,
		fthd_drv.c, fthd_reg.h), reverse engineered from Apple's driver. It
		is GPL-2.0; only facts were taken from it (register offsets, values,
		message layouts and their order), no code.

	The firmware itself (firmware.bin) is Apple's and is not part of
	RenkuOS. The user extracts it from Apple's macOS 10.12.6 update and
	copies it to <system non-packaged data>/firmware/facetimehd/.
*/


#include "facetimehd.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ACPI.h>
#include <FindDirectory.h>
#include <util/AutoLock.h>

extern "C" {
#	include "acpi.h"
}


// #pragma mark - constants [F]


// the firmware runs from the first 8 MB of S2 MEM
#define FIRMWARE_AREA_SIZE		0x800000
// what the firmware is told it may use, firmware included
#define FIRMWARE_MEMORY_SIZE	0x10000000
#define FIRMWARE_HEAP_MIN		0x1000
#define FIRMWARE_HEAP_MAX		0x400000

// written to ISP_FW_HEAP_SIZE once the rings are set up; the firmware
// clears it when it is ready
#define FIRMWARE_HANDSHAKE		0x8042006
#define FIRMWARE_POWER_DOWN		0xf7fbdff9

// the boot-time message queue, cleared before the start
#define ISP_FW_QUEUE_START		0x0128
#define ISP_FW_QUEUE_END		0x0220

// PCI configuration space: interrupt enable and acknowledge
#define PCI_FTHD_IRQ_ACK		0x90
#define PCI_FTHD_IRQ_ENABLE		0x94
#define PCI_FTHD_IRQ_ON			0x200

// the channel table: 256-byte records
#define CHANNEL_RECORD_SIZE		256
#define CHANNEL_NAME_SIZE		64

// ring entries
#define ENTRY_SIZE				64
#define ENTRY_ADDRESS_FLAGS		0
#define ENTRY_REQUEST_SIZE		4
#define ENTRY_RESPONSE_SIZE		8
#define ENTRY_NONE				0xffffffff

// firmware commands, sent on the IO channel
#define CMD_START				0x0
#define CMD_STOP				0x1
#define CMD_PRINT_ENABLE		0x4
#define CMD_POWER_DOWN			0xa
#define CMD_HEADER_SIZE			8
#define CMD_TIMEOUT				2000000		// us

// shared allocations carry a 64-byte header in front [F]
#define SHARED_HEADER_SIZE		64


static acpi_module_info* sACPI;


// #pragma mark - rings


static uint32
entry_address(fthd_channel* channel, uint32 index)
{
	return channel->offset + index * ENTRY_SIZE;
}


/*!	Bit 0 of an entry says whose turn it is; its meaning depends on the
	ring's direction [F].
*/
bool
fthd_ring_host_owns(fthd_device* device, fthd_channel* channel, uint32 entry)
{
	bool flag = (mem_read(device, entry + ENTRY_ADDRESS_FLAGS) & 1) != 0;
	return flag != (channel->type != FTHD_CHANNEL_TO_CAMERA);
}


static void
ring_init(fthd_device* device, fthd_channel* channel)
{
	MutexLocker locker(channel->lock);
	channel->index = 0;
	if (channel->type != FTHD_CHANNEL_TO_CAMERA)
		return;

	for (uint32 i = 0; i < channel->size; i++) {
		uint32 entry = entry_address(channel, i);
		mem_write(device, entry + ENTRY_ADDRESS_FLAGS, 1);
		mem_write(device, entry + ENTRY_REQUEST_SIZE, 0);
		mem_write(device, entry + ENTRY_RESPONSE_SIZE, 0);
	}
}


/*!	Fills the next entry and rings the doorbell. For a ring to the host,
	this hands the entry just received back to the firmware.
*/
status_t
fthd_ring_send(fthd_device* device, fthd_channel* channel, uint32 address,
	uint32 requestSize, uint32 responseSize, uint32* _entry)
{
	MutexLocker locker(channel->lock);

	uint32 entry = entry_address(channel, channel->index);
	if (!fthd_ring_host_owns(device, channel, entry))
		return B_BUSY;

	mem_write(device, entry + ENTRY_REQUEST_SIZE, requestSize);
	mem_write(device, entry + ENTRY_RESPONSE_SIZE, responseSize);
	// S2 MEM is mapped uncached, so these writes reach the device in order
	mem_write(device, entry + ENTRY_ADDRESS_FLAGS, address
		| (channel->type != FTHD_CHANNEL_TO_CAMERA ? 1 : 0));

	if (++channel->index >= channel->size)
		channel->index = 0;
	locker.Unlock();

	isp_write(device, ISP_DOORBELL, 0x10 << channel->source);
	if (_entry != NULL)
		*_entry = entry;
	return B_OK;
}


/*!	Returns the entry the firmware has filled for the host, or ENTRY_NONE.
	It stays current until fthd_ring_send() hands it back.
*/
static uint32
ring_receive(fthd_device* device, fthd_channel* channel)
{
	MutexLocker locker(channel->lock);

	uint32 entry = entry_address(channel, channel->index);
	if (!fthd_ring_host_owns(device, channel, entry))
		return ENTRY_NONE;

	if (channel->type == FTHD_CHANNEL_TO_CAMERA
		&& ++channel->index >= channel->size) {
		channel->index = 0;
	}
	return entry;
}


// #pragma mark - commands


status_t
fthd_isp_command(fthd_device* device, uint16 opcode, const void* request,
	size_t requestLength, void* response, size_t responseLength)
{
	if (device->io == NULL)
		return B_NO_INIT;

	// one command at a time: the firmware answers them in order
	MutexLocker commandLocker(device->command_lock);

	size_t length = CMD_HEADER_SIZE
		+ (requestLength > responseLength ? requestLength : responseLength);
	uint32 buffer;
	status_t status = fthd_memory_alloc(device, length, &buffer);
	if (status != B_OK)
		return status;

	// header: a word the Linux driver leaves zero, the opcode, the status
	mem_write(device, buffer, 0);
	mem_write(device, buffer + 4, opcode);
	if (requestLength > 0) {
		fthd_memory_copy_to(device, buffer + CMD_HEADER_SIZE, request,
			requestLength);
	}

	uint32 entry;
	status = fthd_ring_send(device, device->io, buffer,
		requestLength + CMD_HEADER_SIZE, responseLength + CMD_HEADER_SIZE,
		&entry);
	if (status != B_OK) {
		FTHD_LOG("command 0x%x: IO ring full\n", opcode);
		fthd_memory_free(device, buffer);
		return status;
	}

	// the firmware does not answer the power-down command [F]
	if (opcode == CMD_POWER_DOWN) {
		fthd_memory_free(device, buffer);
		return B_OK;
	}

	bigtime_t start = system_time();
	while (!fthd_ring_host_owns(device, device->io, entry)) {
		if (system_time() - start > CMD_TIMEOUT) {
			FTHD_LOG("command 0x%x: no answer\n", opcode);
			// the firmware may still write to it, so it is not freed
			return B_TIMED_OUT;
		}
		snooze(1000);
	}

	uint16 commandStatus = mem_read(device, buffer + 4) >> 16;
	if (responseLength > 0) {
		uint32 address = mem_read(device, entry + ENTRY_ADDRESS_FLAGS) & ~3;
		fthd_memory_copy_from(device, address + CMD_HEADER_SIZE, response,
			responseLength);
	}
	fthd_memory_free(device, buffer);

	FTHD_LOG("command 0x%x done after %" B_PRId64 " ms, status 0x%x\n",
		opcode, (system_time() - start) / 1000, commandStatus);
	return commandStatus == 0 ? B_OK : B_ERROR;
}


// #pragma mark - messages from the firmware


static void
handle_shared_malloc(fthd_device* device, fthd_channel* channel,
	uint32 entry, bool* _done)
{
	uint32 requestSize = mem_read(device, entry + ENTRY_REQUEST_SIZE);
	uint32 tag = mem_read(device, entry + ENTRY_RESPONSE_SIZE);
	uint32 address = mem_read(device, entry + ENTRY_ADDRESS_FLAGS) & ~3;

	if (address != 0) {
		// the firmware gives back memory it asked for
		fthd_memory_free(device, address - SHARED_HEADER_SIZE);
		atomic_add(&device->shared_allocations, -1);
		fthd_ring_send(device, channel, 0, 0, 0, NULL);
		return;
	}

	if (requestSize == 0) {
		FTHD_LOG("empty shared memory request\n");
		*_done = true;
		return;
	}

	uint32 offset;
	if (fthd_memory_alloc(device, requestSize + SHARED_HEADER_SIZE, &offset)
			!= B_OK) {
		FTHD_LOG("no memory for a shared request of %" B_PRIu32 " bytes\n",
			requestSize);
		*_done = true;
		return;
	}
	atomic_add(&device->shared_allocations, 1);

	// the tag is four characters, but not always printable ones
	char name[5];
	for (int i = 0; i < 4; i++) {
		char c = (char)(tag >> (24 - 8 * i));
		name[i] = c >= 0x20 && c < 0x7f ? c : '.';
	}
	name[4] = '\0';
	FTHD_LOG("firmware allocated %" B_PRIu32 " bytes at 0x%08" B_PRIx32
		" (%s)\n", requestSize, offset + SHARED_HEADER_SIZE, name);
	fthd_ring_send(device, channel, offset + SHARED_HEADER_SIZE, 0, 0, NULL);
}


static void
handle_terminal(fthd_device* device, fthd_channel* channel, uint32 entry)
{
	uint32 requestSize = mem_read(device, entry + ENTRY_REQUEST_SIZE);
	uint32 address = mem_read(device, entry + ENTRY_ADDRESS_FLAGS) & ~3;

	if (address != 0 && requestSize != 0) {
		char text[512];
		size_t length = requestSize < sizeof(text) - 1
			? requestSize : sizeof(text) - 1;
		fthd_memory_copy_from(device, address, text, length);
		text[length] = '\0';
		length = strnlen(text, length);
		while (length > 0
			&& (text[length - 1] == '\n' || text[length - 1] == '\r')) {
			text[--length] = '\0';
		}
		if (length > 0)
			FTHD_LOG("firmware: %s\n", text);
	}

	fthd_ring_send(device, channel, 0, 0, 0, NULL);
}


static void
handle_buffer_returned(fthd_device* device, fthd_channel* channel,
	uint32 entry)
{
	// a filled frame: the entry points to the descriptor list sent with it
	uint32 address = mem_read(device, entry + ENTRY_ADDRESS_FLAGS);
	uint32 responseSize = mem_read(device, entry + ENTRY_RESPONSE_SIZE);
	fthd_stream_frame_returned(device, address & ~3);
	fthd_ring_send(device, channel,
		(responseSize & 0x10000000) != 0 ? (address & ~3) : 0, 0,
		0x80000000, NULL);
}


static void
handle_channel(fthd_device* device, fthd_channel* channel)
{
	// The host waits for these itself, by polling the entry it sent.
	if (channel == device->io || channel == device->buf_h2t
		|| channel == device->debug) {
		return;
	}

	bool done = false;
	for (uint32 i = 0; i < channel->size && !done; i++) {
		uint32 entry = ring_receive(device, channel);
		if (entry == ENTRY_NONE)
			break;

		if (channel == device->shared_malloc)
			handle_shared_malloc(device, channel, entry, &done);
		else if (channel == device->terminal)
			handle_terminal(device, channel, entry);
		else if (channel == device->buf_t2h)
			handle_buffer_returned(device, channel, entry);
		else if (channel == device->io_t2h)
			fthd_ring_send(device, channel, 0, 0, 0, NULL);
		else
			break;
	}
}


static void
handle_pending(fthd_device* device)
{
	for (int32 round = 0; round < 500; round++) {
		uint32 pending = isp_read(device, ISP_IRQ_STATUS);
		if ((pending & ISP_IRQ_CHANNELS) == 0)
			return;

		isp_write(device, ISP_IRQ_CLEAR, pending);
		gPCI->write_pci_config(device->pci.bus, device->pci.device,
			device->pci.function, PCI_FTHD_IRQ_ACK, 4, PCI_FTHD_IRQ_ON);

		for (uint32 i = 0; i < device->channel_count; i++) {
			fthd_channel* channel = &device->channels[i];
			if (channel->source <= 3
				&& (pending & (0x10 << channel->source)) != 0) {
				handle_channel(device, channel);
			}
		}
	}
	FTHD_LOG("interrupt status stuck at 0x%08" B_PRIx32 "\n",
		isp_read(device, ISP_IRQ_STATUS));
}


// #pragma mark - interrupts


static int32
fthd_interrupt(void* data)
{
	fthd_device* device = (fthd_device*)data;
	uint32 pending = isp_read(device, ISP_IRQ_STATUS);
	if ((pending & ISP_IRQ_CHANNELS) == 0)
		return B_UNHANDLED_INTERRUPT;

	// masked until the thread has handled everything
	gPCI->write_pci_config(device->pci.bus, device->pci.device,
		device->pci.function, PCI_FTHD_IRQ_ENABLE, 4, 0);
	release_sem_etc(device->irq_sem, 1, B_DO_NOT_RESCHEDULE);
	return B_INVOKE_SCHEDULER;
}


/*!	Handles the firmware's messages. It also looks every 50 ms on its own,
	so the camera keeps working if its interrupt never arrives.
*/
static status_t
interrupt_thread(void* data)
{
	fthd_device* device = (fthd_device*)data;
	bool reportedPolling = false;

	while (!device->irq_thread_quit) {
		status_t status = acquire_sem_etc(device->irq_sem, 1,
			B_RELATIVE_TIMEOUT, 50000);
		if (device->irq_thread_quit)
			break;

		if (status == B_OK) {
			if (atomic_add(&device->interrupts, 1) == 0)
				FTHD_LOG("first interrupt\n");
		} else if (!reportedPolling
			&& (isp_read(device, ISP_IRQ_STATUS) & ISP_IRQ_CHANNELS) != 0) {
			FTHD_LOG("messages waiting without an interrupt\n");
			reportedPolling = true;
		}

		handle_pending(device);
		gPCI->write_pci_config(device->pci.bus, device->pci.device,
			device->pci.function, PCI_FTHD_IRQ_ENABLE, 4, PCI_FTHD_IRQ_ON);
	}
	return B_OK;
}


static status_t
install_interrupt(fthd_device* device)
{
	const pci_info& info = device->pci;

	device->irq = info.u.h0.interrupt_line;
	device->msi = false;
	uint32 vector;
	if (gPCI->get_msi_count(info.bus, info.device, info.function) >= 1
		&& gPCI->configure_msi(info.bus, info.device, info.function, 1,
			&vector) == B_OK) {
		if (gPCI->enable_msi(info.bus, info.device, info.function) == B_OK) {
			device->irq = vector;
			device->msi = true;
		} else
			gPCI->unconfigure_msi(info.bus, info.device, info.function);
	}

	if (device->irq == 0 || device->irq == 0xff) {
		FTHD_LOG("no interrupt; messages are only polled\n");
	} else if (install_io_interrupt_handler(device->irq, fthd_interrupt,
			device, 0) == B_OK) {
		device->irq_installed = true;
		FTHD_LOG("interrupt %" B_PRIu32 "%s\n", device->irq,
			device->msi ? " (MSI)" : "");

		uint16 command = gPCI->read_pci_config(info.bus, info.device,
			info.function, PCI_command, 2);
		if (device->msi)
			command |= PCI_command_int_disable;
		else
			command &= ~PCI_command_int_disable;
		gPCI->write_pci_config(info.bus, info.device, info.function,
			PCI_command, 2, command);
	} else
		FTHD_LOG("installing the interrupt handler failed; polling\n");

	device->irq_sem = create_sem(0, "facetimehd interrupt");
	if (device->irq_sem < 0)
		return device->irq_sem;

	device->irq_thread_quit = false;
	device->irq_thread = spawn_kernel_thread(interrupt_thread,
		"facetimehd messages", B_REAL_TIME_DISPLAY_PRIORITY, device);
	if (device->irq_thread < 0) {
		delete_sem(device->irq_sem);
		device->irq_sem = -1;
		return device->irq_thread;
	}
	resume_thread(device->irq_thread);

	isp_write(device, ISP_IRQ_ENABLE, 0xf8);
	gPCI->write_pci_config(info.bus, info.device, info.function,
		PCI_FTHD_IRQ_ENABLE, 4, PCI_FTHD_IRQ_ON);
	return B_OK;
}


static void
remove_interrupt(fthd_device* device)
{
	const pci_info& info = device->pci;

	isp_write(device, ISP_IRQ_ENABLE, 0);
	gPCI->write_pci_config(info.bus, info.device, info.function,
		PCI_FTHD_IRQ_ENABLE, 4, 0);

	if (device->irq_thread >= 0) {
		device->irq_thread_quit = true;
		release_sem(device->irq_sem);
		status_t result;
		wait_for_thread(device->irq_thread, &result);
		device->irq_thread = -1;
	}
	if (device->irq_sem >= 0) {
		delete_sem(device->irq_sem);
		device->irq_sem = -1;
	}

	if (device->irq_installed) {
		remove_io_interrupt_handler(device->irq, fthd_interrupt, device);
		device->irq_installed = false;
	}
	if (device->msi) {
		gPCI->disable_msi(info.bus, info.device, info.function);
		gPCI->unconfigure_msi(info.bus, info.device, info.function);
		device->msi = false;
	}
}


// #pragma mark - firmware and sensor


/*!	Opens \a name in the facetimehd firmware folder: the non-packaged one
	first, where the user puts Apple's files, then the packaged one.
*/
int
fthd_open_firmware(const char* name, char* path, size_t pathSize)
{
	static const directory_which kDirectories[] = {
		B_SYSTEM_NONPACKAGED_DATA_DIRECTORY,
		B_SYSTEM_DATA_DIRECTORY
	};

	for (size_t i = 0; i < B_COUNT_OF(kDirectories); i++) {
		if (find_directory(kDirectories[i], -1, false, path, pathSize)
				!= B_OK) {
			continue;
		}
		strlcat(path, "/firmware/facetimehd/", pathSize);
		strlcat(path, name, pathSize);
		int fd = open(path, O_RDONLY);
		if (fd >= 0)
			return fd;
	}

	char folder[B_PATH_NAME_LENGTH];
	if (find_directory(B_SYSTEM_NONPACKAGED_DATA_DIRECTORY, -1, false,
			folder, sizeof(folder)) != B_OK) {
		strlcpy(folder, "?", sizeof(folder));
	}
	FTHD_LOG("%s not found; copy it to %s/firmware/facetimehd/\n", name,
		folder);
	return B_ENTRY_NOT_FOUND;
}


/*!	Copies the file open as \a fd to S2 MEM at \a offset, at most
	\a maxSize bytes.
*/
status_t
fthd_copy_file_to_memory(fthd_device* device, int fd, const char* path,
	uint32 offset, size_t maxSize, size_t* _size)
{
	struct stat stat;
	if (fstat(fd, &stat) != 0 || stat.st_size <= 0
		|| (size_t)stat.st_size > maxSize) {
		FTHD_LOG("%s has a bad size\n", path);
		return B_BAD_DATA;
	}

	const size_t kChunkSize = 64 * 1024;
	uint8* chunk = (uint8*)malloc(kChunkSize);
	if (chunk == NULL)
		return B_NO_MEMORY;

	size_t done = 0;
	status_t status = B_OK;
	while (done < (size_t)stat.st_size) {
		ssize_t bytesRead = read(fd, chunk, kChunkSize);
		if (bytesRead <= 0) {
			status = bytesRead < 0 ? (status_t)bytesRead : B_IO_ERROR;
			break;
		}
		fthd_memory_copy_to(device, offset + done, chunk, bytesRead);
		done += bytesRead;
	}
	free(chunk);

	if (status != B_OK || done != (size_t)stat.st_size) {
		FTHD_LOG("reading %s failed\n", path);
		return status != B_OK ? status : B_IO_ERROR;
	}
	*_size = done;
	return B_OK;
}


static status_t
load_firmware(fthd_device* device)
{
	char path[B_PATH_NAME_LENGTH];
	int fd = fthd_open_firmware("firmware.bin", path, sizeof(path));
	if (fd < 0)
		return fd;

	size_t size;
	status_t status = fthd_copy_file_to_memory(device, fd, path, 0,
		FIRMWARE_AREA_SIZE, &size);
	close(fd);
	if (status != B_OK)
		return status;

	FTHD_LOG("loaded %s, %" B_PRIuSIZE " bytes\n", path, size);
	return B_OK;
}


static acpi_status
find_sensor_power(acpi_handle object, uint32 level, void* context,
	void** _returnValue)
{
	acpi_handle method;
	if (sACPI->get_handle(object, "CMPE", &method) != B_OK)
		return AE_OK;

	*(acpi_handle*)context = object;
	return AE_CTRL_TERMINATE;
}


/*!	The sensor is powered through the ACPI method CMPE on the camera's ACPI
	node [F]. Some machines do without it, so failing is not fatal.
	On the MacBookPro14,1 (\_SB.PCI0.RP05.CMPE) the method takes no
	argument; ACPICA then drops the one passed and warns about it.
*/
static void
set_sensor_power(bool on)
{
	if (sACPI == NULL
		&& get_module(B_ACPI_MODULE_NAME, (module_info**)&sACPI) != B_OK) {
		FTHD_LOG("no ACPI, sensor power not switched\n");
		return;
	}

	acpi_handle systemBus;
	acpi_handle camera = NULL;
	if (sACPI->get_handle(NULL, "\\_SB", &systemBus) == B_OK) {
		sACPI->walk_namespace(systemBus, ACPI_TYPE_DEVICE, 16,
			find_sensor_power, NULL, &camera, NULL);
	}
	if (camera == NULL) {
		FTHD_LOG("no ACPI device with CMPE, sensor power not switched\n");
		return;
	}

	char name[128] = "?";
	acpi_data path = { ACPI_ALLOCATE_BUFFER, NULL };
	if (sACPI->ns_handle_to_pathname(camera, &path) == B_OK
		&& path.pointer != NULL) {
		strlcpy(name, (const char*)path.pointer, sizeof(name));
	}
	free(path.pointer);

	acpi_object_type argument;
	argument.object_type = ACPI_TYPE_INTEGER;
	argument.integer.integer = on ? 1 : 0;
	acpi_objects arguments = { 1, &argument };
	acpi_data result = { ACPI_ALLOCATE_BUFFER, NULL };

	status_t status = sACPI->evaluate_method(camera, "CMPE", &arguments,
		&result);
	char value[32] = "nothing";
	if (status == B_OK && result.pointer != NULL) {
		acpi_object_type* object = (acpi_object_type*)result.pointer;
		if (object->object_type == ACPI_TYPE_INTEGER) {
			snprintf(value, sizeof(value), "%" B_PRIu64,
				object->integer.integer);
		}
	}
	free(result.pointer);

	FTHD_LOG("%s.CMPE(%d): %s, returned %s\n", name, on ? 1 : 0,
		strerror(status), value);
}


// #pragma mark - start


static status_t
wait_for_wake(fthd_device* device, const char* what)
{
	for (int32 i = 0; i < 1000; i++) {
		if ((isp_read(device, ISP_IRQ_STATUS) & ISP_IRQ_CHANNELS) != 0) {
			FTHD_LOG("%s after %" B_PRId32 " ms\n", what, i * 10);
			return B_OK;
		}
		snooze(10000);
	}
	FTHD_LOG("%s: timed out, status 0x%08" B_PRIx32 "\n", what,
		isp_read(device, ISP_IRQ_STATUS));
	return B_TIMED_OUT;
}


static fthd_channel*
find_channel(fthd_device* device, const char* name)
{
	for (uint32 i = 0; i < device->channel_count; i++) {
		if (strcasecmp(device->channels[i].name, name) == 0)
			return &device->channels[i];
	}
	return NULL;
}


static status_t
read_channel_table(fthd_device* device, uint32 table, uint32 count)
{
	for (uint32 i = 0; i < count; i++) {
		uint32 record = table + i * CHANNEL_RECORD_SIZE;
		fthd_channel* channel = &device->channels[i];

		fthd_memory_copy_from(device, record, channel->name,
			CHANNEL_NAME_SIZE);
		channel->name[CHANNEL_NAME_SIZE - 1] = '\0';
		channel->type = mem_read(device, record + CHANNEL_NAME_SIZE);
		channel->source = mem_read(device, record + CHANNEL_NAME_SIZE + 4);
		channel->size = mem_read(device, record + CHANNEL_NAME_SIZE + 8);
		channel->offset = mem_read(device, record + CHANNEL_NAME_SIZE + 12);
		channel->index = 0;
		mutex_init(&channel->lock, "facetimehd channel");
		device->channel_count = i + 1;

		FTHD_LOG("channel %" B_PRIu32 ": %s, type %" B_PRIu32 ", source %"
			B_PRIu32 ", %" B_PRIu32 " entries at 0x%08" B_PRIx32 "\n", i,
			channel->name, channel->type, channel->source, channel->size,
			channel->offset);

		if (channel->size == 0 || channel->size > 4096
			|| (uint64)channel->offset + channel->size * ENTRY_SIZE
				> device->s2_mem_size) {
			FTHD_LOG("channel %s is out of range\n", channel->name);
			return B_BAD_DATA;
		}
	}

	device->terminal = find_channel(device, "TERMINAL");
	device->debug = find_channel(device, "DEBUG");
	device->shared_malloc = find_channel(device, "SHAREDMALLOC");
	device->io = find_channel(device, "IO");
	device->buf_h2t = find_channel(device, "BUF_H2T");
	device->buf_t2h = find_channel(device, "BUF_T2H");
	device->io_t2h = find_channel(device, "IO_T2H");
	if (device->terminal == NULL || device->debug == NULL
		|| device->shared_malloc == NULL || device->io == NULL
		|| device->buf_h2t == NULL || device->buf_t2h == NULL
		|| device->io_t2h == NULL) {
		FTHD_LOG("the firmware lacks one of the seven channels\n");
		return B_BAD_DATA;
	}
	return B_OK;
}


static status_t
start_processor(fthd_device* device)
{
	isp_write(device, ISP_FW_CHAN_CTRL, 0);
	isp_write(device, ISP_FW_QUEUE_CTRL, 0);
	isp_write(device, ISP_FW_SIZE, 0);
	isp_write(device, ISP_FW_HEAP_SIZE, 0);
	isp_write(device, ISP_FW_HEAP_ADDR, 0);
	isp_write(device, ISP_FW_HEAP_SIZE2, 0);
	isp_write(device, ISP_REG_C3018, 0);
	isp_write(device, ISP_FW_ARGS, 0);
	isp_write(device, ISP_IRQ_CLEAR, 0xffffffff);

	for (uint32 offset = ISP_FW_QUEUE_START; offset <= ISP_FW_QUEUE_END;
			offset += 8) {
		isp_write(device, offset, 0xffffffff);
		isp_write(device, offset + 4, 0);
	}

	isp_write(device, ISP_REG_40008, 0x80000000);
	isp_write(device, ISP_REG_40004, 1);

	status_t status = wait_for_wake(device, "processor woke up");
	if (status != B_OK)
		return status;
	isp_write(device, ISP_IRQ_CLEAR, 0xffffffff);

	uint32 channelCount = isp_read(device, ISP_FW_CHAN_CTRL);
	uint32 queueSize = isp_read(device, ISP_FW_QUEUE_CTRL) + 1;
	uint32 heapSize = isp_read(device, ISP_FW_HEAP_SIZE);
	FTHD_LOG("%" B_PRIu32 " channels, queue %" B_PRIu32 " bytes, heap "
		"request %" B_PRIu32 " KB\n", channelCount, queueSize,
		heapSize / 1024);

	if (channelCount == 0 || channelCount > FTHD_MAX_CHANNELS) {
		FTHD_LOG("bad channel count\n");
		return B_BAD_DATA;
	}
	if (heapSize == 0) {
		// The Linux driver has a path for this, but it never reads the
		// channels on it, so the firmware cannot be used.
		FTHD_LOG("the firmware asked for no heap; not supported\n");
		return B_NOT_SUPPORTED;
	}
	if (heapSize < FIRMWARE_HEAP_MIN)
		heapSize = FIRMWARE_HEAP_MIN;
	if (heapSize > FIRMWARE_HEAP_MAX) {
		FTHD_LOG("heap request too big\n");
		return B_NO_MEMORY;
	}

	uint32 queue;
	uint32 heap;
	uint32 arguments;
	if (fthd_memory_alloc(device, queueSize, &queue) != B_OK
		|| fthd_memory_alloc(device, heapSize, &heap) != B_OK
		|| fthd_memory_alloc(device, 12, &arguments) != B_OK) {
		return B_NO_MEMORY;
	}

	isp_write(device, ISP_FW_CHAN_CTRL, 0);
	isp_write(device, ISP_FW_QUEUE_CTRL, queue);
	isp_write(device, ISP_FW_SIZE, FIRMWARE_AREA_SIZE);
	isp_write(device, ISP_FW_HEAP_SIZE,
		FIRMWARE_MEMORY_SIZE - FIRMWARE_AREA_SIZE);
	isp_write(device, ISP_FW_HEAP_ADDR, heap);
	isp_write(device, ISP_FW_HEAP_SIZE2, heapSize);

	// the firmware's arguments: an unknown 2, then fw_arg and the full
	// statistics mode, both 0 [F]
	mem_write(device, arguments, 2);
	mem_write(device, arguments + 4, 0);
	mem_write(device, arguments + 8, 0);
	isp_write(device, ISP_FW_ARGS, arguments);

	isp_write(device, ISP_DOORBELL, 0x10);
	status = wait_for_wake(device, "firmware started");
	if (status != B_OK)
		return status;

	uint32 table = isp_read(device, ISP_FW_CHAN_CTRL);
	FTHD_LOG("channel table at 0x%08" B_PRIx32 "\n", table);
	status = read_channel_table(device, table, channelCount);
	if (status != B_OK)
		return status;

	for (uint32 i = 0; i < device->channel_count; i++)
		ring_init(device, &device->channels[i]);

	isp_write(device, ISP_FW_HEAP_SIZE, FIRMWARE_HANDSHAKE);
	for (int32 i = 0; i < 1000; i++) {
		if (isp_read(device, ISP_FW_HEAP_SIZE) == 0) {
			FTHD_LOG("firmware ready after %" B_PRId32 " ms\n", i * 10);
			return B_OK;
		}
		snooze(10000);
	}
	FTHD_LOG("firmware did not answer the handshake\n");
	return B_TIMED_OUT;
}


static void
stop_processor(fthd_device* device)
{
	isp_write(device, ISP_REG_40004, 0);
	isp_write(device, ISP_IRQ_ENABLE, 0);
	isp_write(device, 0xc0008, 0xffffffff);
	isp_write(device, 0xc000c, 0xffffffff);
	isp_write(device, 0xc0010, 0xffffffff);
	isp_write(device, 0xc1004, 0);
	isp_write(device, 0xc100c, 0xffffffff);
	isp_write(device, 0xc1014, 0xffffffff);
	isp_write(device, 0xc101c, 0xffffffff);
	isp_write(device, 0xc1024, 0xffffffff);
	snooze(1000);

	for (uint32 offset = 0xc0000; offset <= 0xc0024; offset += 4)
		isp_write(device, offset, 0);
	isp_write(device, ISP_IRQ_CLEAR, 0xffffffff);
	isp_write(device, ISP_DOORBELL, 0);
}


static void
free_channels(fthd_device* device)
{
	for (uint32 i = 0; i < device->channel_count; i++)
		mutex_destroy(&device->channels[i].lock);
	device->channel_count = 0;
	device->terminal = device->debug = device->shared_malloc = device->io
		= device->buf_h2t = device->buf_t2h = device->io_t2h = NULL;
}


// #pragma mark - public


status_t
fthd_isp_init(fthd_device* device)
{
	device->irq_sem = -1;
	device->irq_thread = -1;

	status_t status = fthd_memory_reserve(device, 0, FIRMWARE_AREA_SIZE);
	if (status != B_OK)
		return status;

	status = load_firmware(device);
	if (status != B_OK)
		return status;

	gPCI->set_powerstate(device->pci.bus, device->pci.device,
		device->pci.function, PCI_pm_state_d0);
	snooze(10000);

	set_sensor_power(true);
	// the sensor's power rail needs time to settle [F]
	snooze(100000);

	status = start_processor(device);
	if (status == B_OK)
		status = install_interrupt(device);
	if (status != B_OK) {
		stop_processor(device);
		free_channels(device);
		return status;
	}
	device->isp_up = true;

	status = fthd_isp_command(device, CMD_START, NULL, 0, NULL, 0);
	if (status != B_OK) {
		FTHD_LOG("the firmware did not start: %s\n", strerror(status));
		return status;
	}

	uint32 enable = 1;
	status = fthd_isp_command(device, CMD_PRINT_ENABLE, &enable, sizeof(enable),
		NULL, 0);
	if (status != B_OK)
		FTHD_LOG("turning the firmware log on failed: %s\n", strerror(status));

	FTHD_LOG("firmware running, %" B_PRId32 " shared allocations\n",
		atomic_get(&device->shared_allocations));

	fthd_stream_query_sensor(device);
	return B_OK;
}


void
fthd_isp_uninit(fthd_device* device)
{
	if (device->isp_up) {
		fthd_isp_command(device, CMD_STOP, NULL, 0, NULL, 0);

		isp_write(device, ISP_FW_CHAN_CTRL, FIRMWARE_POWER_DOWN);
		fthd_isp_command(device, CMD_POWER_DOWN, NULL, 0, NULL, 0);
		bool down = false;
		for (int32 i = 0; i < 100 && !down; i++) {
			down = isp_read(device, ISP_FW_CHAN_CTRL) == FIRMWARE_HANDSHAKE;
			if (!down)
				snooze(10000);
		}
		FTHD_LOG("firmware %s\n", down ? "powered down"
			: "did not confirm the power-down");

		remove_interrupt(device);
		stop_processor(device);
		free_channels(device);
		device->isp_up = false;
	}

	if (sACPI != NULL) {
		put_module(B_ACPI_MODULE_NAME);
		sACPI = NULL;
	}
}
