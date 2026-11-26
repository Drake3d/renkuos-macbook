/*
 * Copyright 2026, RenkuOS Contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


/*!	Video from the camera: asks the firmware about the sensor, sets up a
	1280x720 YUYV stream, and lets the camera write frames into buffers in
	host memory.

	The camera reaches host memory through a page table in BAR 0 (4096
	entries of 4 KB pages, one host page number each); the firmware
	addresses page table entry n as 0xc0000000 + n * 4096. Each frame
	buffer is described by a 0x180-byte descriptor list, sent on the
	BUF_H2T ring; the camera hands it back on BUF_T2H once filled.

	Sources, as noted at each step:
	[F]	The patjak/facetimehd Linux driver (fthd_isp.c, fthd_isp.h,
		fthd_buffer.c, fthd_buffer.h, fthd_v4l2.c), reverse engineered from
		Apple's driver. It is GPL-2.0; only facts were taken from it
		(command numbers, payload layouts, values and their order), no code.
	[W]	Its wiki page "Extracting the sensor calibration files".
*/


#include "facetimehd.h"
#include "facetimehd_ioctl.h"

#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <driver_settings.h>
#include <util/AutoLock.h>


// #pragma mark - firmware commands [F]


#define CMD_CONFIG_GET						0x0003
#define CMD_CH_START						0x0100
#define CMD_CH_STOP							0x0101
#define CMD_CH_BUFFER_RETURN				0x0104
#define CMD_CH_CAMERA_CONFIG_GET			0x0106
#define CMD_CH_CAMERA_CONFIG_SELECT			0x0107
#define CMD_CH_INFO_GET						0x010d
#define CMD_CH_BUFFER_RECYCLE_MODE_SET		0x010e
#define CMD_CH_BUFFER_RECYCLE_START			0x010f
#define CMD_CH_SET_FILE_LOAD				0x0111
#define CMD_CH_SIF_PIXEL_FORMAT_SET			0x0115
#define CMD_CH_CAMERA_ERR_HANDLE_CONFIG		0x0124
#define CMD_CH_AE_FRAME_RATE_MAX_SET		0x0208
#define CMD_CH_AE_FRAME_RATE_MIN_SET		0x020a
#define CMD_CH_AE_SPEED_SET					0x0218
#define CMD_CH_AE_STABILITY_SET				0x021a
#define CMD_CH_AE_STABILITY_TO_STABLE_SET	0x0229
#define CMD_CH_CROP_SET						0x0801
#define CMD_CH_OUTPUT_CONFIG_SET			0x0b01
#define CMD_CH_SCALER_BRIGHTNESS_SET		0x0b02
#define CMD_CH_SCALER_CONTRAST_SET			0x0b03
#define CMD_CH_SCALER_SATURATION_SET		0x0b04
#define CMD_CH_SCALER_HUE_SET				0x0b05
#define CMD_CH_AWB_START					0x0300
#define CMD_CH_AWB_STOP						0x0301
#define CMD_CH_DRC_START					0x0c00
#define CMD_CH_FACE_DETECTION_START			0x0d00
#define CMD_CH_FACE_DETECTION_STOP			0x0d01
#define CMD_CH_FACE_DETECTION_DISABLE		0x0d04
#define CMD_CH_FACE_DETECTION_ENABLE		0x0d05
#define CMD_APPLE_CH_STREAMING_MODE_SET		0x8106
#define CMD_APPLE_CH_AE_METERING_MODE_SET	0x8206
#define CMD_APPLE_CH_TEMPORAL_FILTER_START	0xc100
#define CMD_APPLE_CH_TEMPORAL_FILTER_STOP	0xc101
#define CMD_APPLE_CH_MOTION_HISTORY_START	0xc102
#define CMD_APPLE_CH_MOTION_HISTORY_STOP	0xc103
#define CMD_APPLE_CH_TEMPORAL_FILTER_ENABLE	0xc113
#define CMD_APPLE_CH_TEMPORAL_FILTER_DISABLE 0xc114
#define CMD_APPLE_CH_TONE_CURVE_ADAPTATION_START 0xc300

// sizes of the query payloads
#define CONFIG_SIZE				32
#define CHANNEL_INFO_SIZE		158
#define CAMERA_CONFIG_SIZE		96

// frame rates are in 1/256 frames per second
#define FRAME_RATE				(30 * 256)
// the slowest the auto exposure may go in low light [F]
#define FRAME_RATE_LOW_LIGHT	(5 * 256)

// the set files are at most this big [W]
#define SET_FILE_MAX_SIZE		(64 * 1024)

// output pixel format: 1 = YUYV
#define PIXEL_FORMAT_YUYV		1

#define STREAM_WIDTH			1280
#define STREAM_HEIGHT			720


// #pragma mark - frame buffers [F]


#define S2_PAGE_TABLE			0x9000
#define S2_PAGE_TABLE_ENTRIES	0x1000
#define CAMERA_ADDRESS_BASE		0xc0000000

#define DESCRIPTOR_LIST_SIZE	0x180
#define DESCRIPTOR_SIZE			40
#define DESCRIPTOR_LIST_COUNT	4
// in the list: field0 and count, then the descriptors
#define LIST_FIELD0				0
#define LIST_COUNT				4
#define LIST_DESCRIPTORS		8
// in a descriptor
#define DESC_ADDRESS0			0
#define DESC_COUNT				24
#define DESC_POOL				28
#define DESC_TAG				32

#define BUFFER_H2T_RESPONSE		0x30000000
#define BUFFER_SEND_TIMEOUT		2000000		// us


// #pragma mark - helpers


/*!	Sends a channel command. Their payloads go both ways, so the answer is
	read back into a scratch copy of the same size, as the Linux driver
	does [F].
*/
static status_t
channel_command(fthd_device* device, uint16 opcode, const uint32* payload,
	size_t length)
{
	uint8 response[64];
	if (length > sizeof(response))
		return B_BAD_VALUE;

	status_t status = fthd_isp_command(device, opcode, payload, length,
		response, length);
	if (status != B_OK) {
		FTHD_LOG("command 0x%04x failed: %s\n", opcode, strerror(status));
	}
	return status;
}


static status_t
channel_command(fthd_device* device, uint16 opcode)
{
	const uint32 payload[] = { 0 };
	return channel_command(device, opcode, payload, sizeof(payload));
}


static status_t
channel_command(fthd_device* device, uint16 opcode, uint32 value)
{
	// channel 0, then a u32 - or a u16 or two u8 in the low bytes, padded
	const uint32 payload[] = { 0, value };
	return channel_command(device, opcode, payload, sizeof(payload));
}


static void
log_hex(const char* what, const uint8* data, size_t length)
{
	char line[3 * 32 + 1];
	for (size_t offset = 0; offset < length; offset += 32) {
		size_t count = length - offset < 32 ? length - offset : 32;
		for (size_t i = 0; i < count; i++) {
			static const char kHex[] = "0123456789abcdef";
			line[i * 3] = kHex[data[offset + i] >> 4];
			line[i * 3 + 1] = kHex[data[offset + i] & 0xf];
			line[i * 3 + 2] = ' ';
		}
		line[count * 3] = '\0';
		FTHD_LOG("%s %03" B_PRIxSIZE ": %s\n", what, offset, line);
	}
}


static uint16
read16(const uint8* data, size_t offset)
{
	return data[offset] | (data[offset + 1] << 8);
}


static uint32
read32(const uint8* data, size_t offset)
{
	return read16(data, offset) | ((uint32)read16(data, offset + 2) << 16);
}


// #pragma mark - sensor


/*!	Takes the picture settings the Camera preferences saved (see
	facetimehd_ioctl.h); values out of range are ignored.
*/
static void
load_saved_controls(fthd_device* device)
{
	void* settings = load_driver_settings(FTHD_SETTINGS_NAME);
	if (settings == NULL) {
		FTHD_LOG("no saved picture settings (%s), using the defaults\n",
			"~/config/settings/kernel/drivers/" FTHD_SETTINGS_NAME);
		return;
	}

	int32 loaded = 0;
	for (uint32 id = 0; id < FTHD_CONTROL_COUNT; id++) {
		const char* text = get_driver_parameter(settings,
			kFTHDControlNames[id], NULL, NULL);
		if (text == NULL)
			continue;

		char* end;
		long value = strtol(text, &end, 10);
		bool isSwitch = id == FTHD_CONTROL_AUTO_WHITE_BALANCE
			|| id == FTHD_CONTROL_LOW_LIGHT;
		if (end == text || *end != '\0' || value < 0
			|| value > (isSwitch ? 1 : 255)) {
			FTHD_LOG("saved setting %s=%s ignored\n", kFTHDControlNames[id],
				text);
			continue;
		}
		device->controls[id] = value;
		loaded++;
	}
	unload_driver_settings(settings);

	FTHD_LOG("picture settings: brightness %" B_PRId32 ", contrast %" B_PRId32
		", saturation %" B_PRId32 ", hue %" B_PRId32 ", white balance %s, "
		"low light %s (%" B_PRId32 " saved)\n",
		device->controls[FTHD_CONTROL_BRIGHTNESS],
		device->controls[FTHD_CONTROL_CONTRAST],
		device->controls[FTHD_CONTROL_SATURATION],
		device->controls[FTHD_CONTROL_HUE],
		device->controls[FTHD_CONTROL_AUTO_WHITE_BALANCE] != 0 ? "auto" : "off",
		device->controls[FTHD_CONTROL_LOW_LIGHT] != 0 ? "on" : "off", loaded);
}


void
fthd_stream_init(fthd_device* device)
{
	mutex_init(&device->command_lock, "facetimehd command");
	mutex_init(&device->stream_lock, "facetimehd stream");
	mutex_init(&device->frame_lock, "facetimehd frames");
	device->frame_sem = create_sem(0, "facetimehd frames");
	device->frame_area = -1;
	device->width = STREAM_WIDTH;
	device->height = STREAM_HEIGHT;

	// the Linux driver's defaults [F]
	device->controls[FTHD_CONTROL_BRIGHTNESS] = 128;
	device->controls[FTHD_CONTROL_CONTRAST] = 128;
	device->controls[FTHD_CONTROL_SATURATION] = 128;
	device->controls[FTHD_CONTROL_HUE] = 128;
	device->controls[FTHD_CONTROL_AUTO_WHITE_BALANCE] = 1;
	device->controls[FTHD_CONTROL_LOW_LIGHT] = 0;

	load_saved_controls(device);
}


void
fthd_stream_uninit(fthd_device* device)
{
	if (device->frame_sem >= 0)
		delete_sem(device->frame_sem);
	device->frame_sem = -1;
	mutex_destroy(&device->frame_lock);
	mutex_destroy(&device->stream_lock);
	mutex_destroy(&device->command_lock);
}


void
fthd_stream_query_sensor(fthd_device* device)
{
	uint8 config[CONFIG_SIZE] = {};
	if (fthd_isp_command(device, CMD_CONFIG_GET, config, sizeof(config),
			config, sizeof(config)) == B_OK) {
		log_hex("camera config", config, sizeof(config));
	}

	uint8 info[CHANNEL_INFO_SIZE] = {};
	if (fthd_isp_command(device, CMD_CH_INFO_GET, info, sizeof(info), info,
			sizeof(info)) == B_OK) {
		log_hex("channel info", info, sizeof(info));
		device->sensor_id0 = read16(info, 0x10);
		device->sensor_id1 = read16(info, 0x18);
		device->sensor_count = read32(info, 0x58);

		char serial[19];
		memcpy(serial, info + 0x8c, 18);
		serial[18] = '\0';
		for (int i = 0; serial[i] != '\0'; i++) {
			if (serial[i] < 0x20 || serial[i] >= 0x7f)
				serial[i] = '.';
		}
		FTHD_LOG("sensor %04x %04x, %" B_PRIu32 " sensor(s), module %s\n",
			device->sensor_id0, device->sensor_id1, device->sensor_count,
			serial);
	}

	// sensor 0's native size: the first two u16 of its configuration [F]
	uint32 cameraConfig[CAMERA_CONFIG_SIZE / 4] = {};
	if (fthd_isp_command(device, CMD_CH_CAMERA_CONFIG_GET, cameraConfig,
			sizeof(cameraConfig), cameraConfig, sizeof(cameraConfig))
			== B_OK) {
		const uint8* data = (const uint8*)cameraConfig;
		log_hex("sensor config", data, sizeof(cameraConfig));
		device->sensor_width = read16(data, 8);
		device->sensor_height = read16(data, 10);
		FTHD_LOG("sensor size %" B_PRIu32 "x%" B_PRIu32 "\n",
			device->sensor_width, device->sensor_height);
	}

	// The calibration ("set file") this sensor needs, by the Linux driver's
	// table [F]. Loading it comes later; without it colours are off.
	const char* setFile = NULL;
	switch (device->sensor_id1) {
		case 0x164: setFile = "8221_01XX.dat"; break;
		case 0x190: setFile = "1222_01XX.dat"; break;
		case 0x248:
			setFile = (device->sensor_id0 >> 4) == 5
				? "1575_01XX.dat" : "1571_01XX.dat";
			break;
		case 0x8830: setFile = "9112_01XX.dat"; break;
	}
	if (setFile == NULL) {
		FTHD_LOG("no set file known for this sensor; colours will be off\n");
		return;
	}

	// The calibration for this sensor: Apple's, from its Boot Camp camera
	// driver, which the user extracts [W]. Without it colours are off.
	char path[B_PATH_NAME_LENGTH];
	int fd = fthd_open_firmware(setFile, path, sizeof(path));
	if (fd < 0)
		return;

	uint32 offset;
	size_t size = 0;
	status_t status = fthd_memory_alloc(device, SET_FILE_MAX_SIZE, &offset);
	if (status != B_OK) {
		close(fd);
		FTHD_LOG("no memory for %s\n", path);
		return;
	}
	status = fthd_copy_file_to_memory(device, fd, path, offset,
		SET_FILE_MAX_SIZE, &size);
	close(fd);
	if (status != B_OK) {
		fthd_memory_free(device, offset);
		FTHD_LOG("loading %s failed: %s\n", path, strerror(status));
		return;
	}

	// an unknown word, the file's address and its length [F]
	const uint32 load[] = { 0, offset, (uint32)size };
	status = fthd_isp_command(device, CMD_CH_SET_FILE_LOAD, load,
		sizeof(load), NULL, 0);
	if (status != B_OK) {
		FTHD_LOG("the firmware refused %s: %s\n", path, strerror(status));
		fthd_memory_free(device, offset);
		return;
	}

	// the firmware keeps using it, so it stays
	device->set_file = offset;
	device->set_file_seen = true;
	FTHD_LOG("loaded set file %s, %" B_PRIuSIZE " bytes\n", path, size);
}


// #pragma mark - picture controls


static status_t
apply_control(fthd_device* device, uint32 id, int32 value)
{
	switch (id) {
		case FTHD_CONTROL_BRIGHTNESS:
			return channel_command(device, CMD_CH_SCALER_BRIGHTNESS_SET,
				value);
		case FTHD_CONTROL_CONTRAST:
			return channel_command(device, CMD_CH_SCALER_CONTRAST_SET, value);
		case FTHD_CONTROL_SATURATION:
			return channel_command(device, CMD_CH_SCALER_SATURATION_SET,
				value);
		case FTHD_CONTROL_HUE:
			return channel_command(device, CMD_CH_SCALER_HUE_SET, value);
		case FTHD_CONTROL_AUTO_WHITE_BALANCE:
			return channel_command(device,
				value != 0 ? CMD_CH_AWB_START : CMD_CH_AWB_STOP);
		case FTHD_CONTROL_LOW_LIGHT:
			return channel_command(device, CMD_CH_AE_FRAME_RATE_MIN_SET,
				value != 0 ? FRAME_RATE_LOW_LIGHT : FRAME_RATE);
	}
	return B_BAD_VALUE;
}


status_t
fthd_stream_set_control(fthd_device* device, uint32 id, int32 value)
{
	if (id >= FTHD_CONTROL_COUNT)
		return B_BAD_VALUE;
	if (id == FTHD_CONTROL_AUTO_WHITE_BALANCE || id == FTHD_CONTROL_LOW_LIGHT)
		value = value != 0 ? 1 : 0;
	else if (value < 0 || value > 255)
		return B_BAD_VALUE;

	MutexLocker locker(device->stream_lock);
	device->controls[id] = value;
	if (!device->streaming)
		return B_OK;
	return apply_control(device, id, value);
}


// #pragma mark - frame buffers


static status_t
map_frames(fthd_device* device)
{
	size_t total = device->frame_size * FTHD_FRAME_BUFFERS;
	uint32 pages = total / B_PAGE_SIZE;
	if (pages > S2_PAGE_TABLE_ENTRIES)
		return B_NO_MEMORY;

	void* address;
	device->frame_area = create_area("facetimehd frames", &address,
		B_ANY_KERNEL_ADDRESS, total, B_FULL_LOCK,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (device->frame_area < 0)
		return device->frame_area;
	device->frame_memory = (uint8*)address;
	memset(address, 0, total);

	// The table holds 32-bit page numbers, so pages below 16 TB only.
	const int32 kRuns = 64;
	physical_entry runs[kRuns];
	uint32 page = 0;
	size_t done = 0;
	while (done < total) {
		uint32 count = kRuns;
		status_t status = get_memory_map_etc(B_SYSTEM_TEAM,
			device->frame_memory + done, total - done, runs, &count);
		if (status != B_OK && status != B_BUFFER_OVERFLOW)
			return status;
		if (count == 0)
			return B_ERROR;

		for (uint32 i = 0; i < count; i++) {
			for (phys_size_t offset = 0; offset < runs[i].size;
					offset += B_PAGE_SIZE) {
				phys_addr_t physical = runs[i].address + offset;
				if ((physical >> 12) > 0xffffffff || page >= pages)
					return B_BAD_ADDRESS;
				s2_write(device, S2_PAGE_TABLE + page * 4,
					(uint32)(physical >> 12));
				page++;
			}
			done += runs[i].size;
		}
	}
	device->mapped_pages = page;

	uint32 framePages = device->frame_size / B_PAGE_SIZE;
	for (int32 i = 0; i < FTHD_FRAME_BUFFERS; i++) {
		fthd_frame& frame = device->frames[i];
		frame.state = FTHD_FRAME_FREE;
		frame.page_index = i * framePages;
		status_t status = fthd_memory_alloc(device, DESCRIPTOR_LIST_SIZE,
			&frame.descriptor);
		if (status != B_OK)
			return status;
	}

	FTHD_LOG("%d frame buffers of %" B_PRIuSIZE " KB, %" B_PRIu32
		" pages mapped\n", FTHD_FRAME_BUFFERS, device->frame_size / 1024,
		page);
	return B_OK;
}


/*!	Frees what map_frames() set up, but only if the camera holds none of
	the buffers any more: otherwise it could still write into them.
*/
static void
unmap_frames(fthd_device* device)
{
	bool held = false;
	for (int32 i = 0; i < FTHD_FRAME_BUFFERS; i++) {
		if (device->frames[i].state == FTHD_FRAME_QUEUED)
			held = true;
	}
	if (held) {
		FTHD_LOG("the camera still holds frame buffers; keeping them\n");
		device->frame_area = -1;
		device->frame_memory = NULL;
		return;
	}

	for (uint32 page = 0; page < device->mapped_pages; page++)
		s2_write(device, S2_PAGE_TABLE + page * 4, 0);
	device->mapped_pages = 0;

	for (int32 i = 0; i < FTHD_FRAME_BUFFERS; i++) {
		fthd_frame& frame = device->frames[i];
		if (frame.descriptor != 0)
			fthd_memory_free(device, frame.descriptor);
		frame.descriptor = 0;
		frame.state = FTHD_FRAME_FREE;
	}

	if (device->frame_area >= 0)
		delete_area(device->frame_area);
	device->frame_area = -1;
	device->frame_memory = NULL;
}


static status_t
queue_frame(fthd_device* device, int32 index)
{
	fthd_frame& frame = device->frames[index];

	uint8 list[DESCRIPTOR_LIST_SIZE] = {};
	uint32 address = CAMERA_ADDRESS_BASE | (frame.page_index << 12);
	uint64 tag = index + 1;
	uint32 one = 1;
	uint32 pool = 2;
	memcpy(list + LIST_FIELD0, &one, 4);
	memcpy(list + LIST_COUNT, &one, 4);
	memcpy(list + LIST_DESCRIPTORS + DESC_ADDRESS0, &address, 4);
	memcpy(list + LIST_DESCRIPTORS + DESC_COUNT, &one, 4);
	memcpy(list + LIST_DESCRIPTORS + DESC_POOL, &pool, 4);
	memcpy(list + LIST_DESCRIPTORS + DESC_TAG, &tag, 8);
	fthd_memory_copy_to(device, frame.descriptor, list, sizeof(list));

	{
		MutexLocker locker(device->frame_lock);
		frame.state = FTHD_FRAME_QUEUED;
	}

	uint32 entry;
	status_t status = fthd_ring_send(device, device->buf_h2t,
		frame.descriptor, DESCRIPTOR_LIST_SIZE, BUFFER_H2T_RESPONSE, &entry);
	if (status == B_OK) {
		bigtime_t start = system_time();
		while (!fthd_ring_host_owns(device, device->buf_h2t, entry)) {
			if (system_time() - start > BUFFER_SEND_TIMEOUT) {
				status = B_TIMED_OUT;
				break;
			}
			snooze(500);
		}
	}

	if (status != B_OK) {
		FTHD_LOG("queueing frame buffer %" B_PRId32 " failed: %s\n", index,
			strerror(status));
		MutexLocker locker(device->frame_lock);
		if (frame.state == FTHD_FRAME_QUEUED && status == B_BUSY)
			frame.state = FTHD_FRAME_FREE;
	}
	return status;
}


void
fthd_stream_frame_returned(fthd_device* device, uint32 descriptorList)
{
	if ((uint64)descriptorList + DESCRIPTOR_LIST_SIZE > device->s2_mem_size)
		return;

	uint32 count = mem_read(device, descriptorList + LIST_COUNT);
	if (count > DESCRIPTOR_LIST_COUNT)
		count = DESCRIPTOR_LIST_COUNT;

	int32 filled = 0;
	for (uint32 i = 0; i < count; i++) {
		uint32 tag = mem_read(device, descriptorList + LIST_DESCRIPTORS
			+ i * DESCRIPTOR_SIZE + DESC_TAG);
		if (tag < 1 || tag > FTHD_FRAME_BUFFERS) {
			FTHD_LOG("returned buffer with unknown tag %" B_PRIu32 "\n", tag);
			continue;
		}

		MutexLocker locker(device->frame_lock);
		fthd_frame& frame = device->frames[tag - 1];
		if (frame.state != FTHD_FRAME_QUEUED)
			continue;
		frame.state = FTHD_FRAME_DONE;
		frame.sequence = device->sequence++;
		frame.time = system_time();
		filled++;
	}

	if (filled == 0)
		return;

	int32 previous = atomic_add(&device->frames_received, filled);
	if (previous == 0) {
		FTHD_LOG("first frame after %" B_PRId64 " ms\n",
			(system_time() - device->stream_start) / 1000);
	}
	release_sem_etc(device->frame_sem, filled, B_DO_NOT_RESCHEDULE);
}


// #pragma mark - stream


static status_t
start_channel(fthd_device* device)
{
	uint32 width = device->width;
	uint32 height = device->height;

	// the largest centred window with the output's shape, even width [F]
	uint32 sensorWidth = device->sensor_width != 0
		? device->sensor_width : width;
	uint32 sensorHeight = device->sensor_height != 0
		? device->sensor_height : height;
	uint32 cropWidth = sensorWidth;
	uint32 cropHeight = sensorWidth * height / width;
	if (cropHeight > sensorHeight) {
		cropHeight = sensorHeight;
		cropWidth = sensorHeight * width / height;
	}
	cropWidth &= ~1u;
	uint32 cropX = (sensorWidth - cropWidth) / 2;
	uint32 cropY = (sensorHeight - cropHeight) / 2;
	FTHD_LOG("stream %" B_PRIu32 "x%" B_PRIu32 " from a %" B_PRIu32 "x%"
		B_PRIu32 " window at %" B_PRIu32 ",%" B_PRIu32 "\n", width, height,
		cropWidth, cropHeight, cropX, cropY);

	const uint32 select[] = { 0, 0 };
	const uint32 crop[] = { 0, cropX, cropY, cropWidth, cropHeight };
	// channel, Y width, height, unknown, format, chroma width?, width,
	// unknown 0x7ff [F]
	const uint32 output[] = { 0, width, height, 0, PIXEL_FORMAT_YUYV,
		width * 2, width, 0x7ff };

	status_t status;
	if ((status = channel_command(device, CMD_CH_CAMERA_CONFIG_SELECT,
			select, sizeof(select))) != B_OK
		|| (status = channel_command(device, CMD_CH_CROP_SET, crop,
			sizeof(crop))) != B_OK
		|| (status = channel_command(device, CMD_CH_OUTPUT_CONFIG_SET,
			output, sizeof(output))) != B_OK
		|| (status = channel_command(device, CMD_CH_BUFFER_RECYCLE_MODE_SET,
			1)) != B_OK
		|| (status = channel_command(device, CMD_CH_BUFFER_RECYCLE_START,
			0)) != B_OK
		|| (status = channel_command(device,
			CMD_APPLE_CH_AE_METERING_MODE_SET, 3)) != B_OK
		|| (status = channel_command(device, CMD_CH_DRC_START)) != B_OK
		|| (status = channel_command(device,
			CMD_APPLE_CH_TONE_CURVE_ADAPTATION_START)) != B_OK
		|| (status = channel_command(device, CMD_CH_AE_SPEED_SET, 60))
			!= B_OK
		|| (status = channel_command(device, CMD_CH_AE_STABILITY_SET, 75))
			!= B_OK
		|| (status = channel_command(device,
			CMD_CH_AE_STABILITY_TO_STABLE_SET, 8)) != B_OK
		// two u8 parameters, 1 and 1
		|| (status = channel_command(device, CMD_CH_SIF_PIXEL_FORMAT_SET,
			0x0101)) != B_OK
		// two u16 parameters, 2 and 1
		|| (status = channel_command(device, CMD_CH_CAMERA_ERR_HANDLE_CONFIG,
			2 | (1 << 16))) != B_OK
		|| (status = channel_command(device, CMD_CH_FACE_DETECTION_ENABLE))
			!= B_OK
		|| (status = channel_command(device, CMD_CH_FACE_DETECTION_START))
			!= B_OK
		|| (status = channel_command(device, CMD_CH_AE_FRAME_RATE_MAX_SET,
			FRAME_RATE)) != B_OK
		|| (status = channel_command(device, CMD_CH_AE_FRAME_RATE_MIN_SET,
			device->controls[FTHD_CONTROL_LOW_LIGHT] != 0
				? FRAME_RATE_LOW_LIGHT : FRAME_RATE)) != B_OK
		|| (status = channel_command(device,
			CMD_APPLE_CH_TEMPORAL_FILTER_START)) != B_OK
		|| (status = channel_command(device,
			CMD_APPLE_CH_MOTION_HISTORY_START)) != B_OK
		|| (status = channel_command(device,
			CMD_APPLE_CH_TEMPORAL_FILTER_ENABLE)) != B_OK
		|| (status = channel_command(device, CMD_APPLE_CH_STREAMING_MODE_SET,
			0)) != B_OK
		|| (status = channel_command(device, CMD_CH_START)) != B_OK) {
		return status;
	}

	// Starting the channel resets the picture controls: set them again
	// once it runs [F].
	for (uint32 id = 0; id < FTHD_CONTROL_COUNT; id++)
		apply_control(device, id, device->controls[id]);

	// the auto exposure needs a moment to settle [F]
	snooze(200000);
	return B_OK;
}


static void
stop_channel(fthd_device* device)
{
	channel_command(device, CMD_CH_STOP);
	channel_command(device, CMD_CH_BUFFER_RETURN);
	channel_command(device, CMD_CH_FACE_DETECTION_STOP);
	channel_command(device, CMD_CH_FACE_DETECTION_DISABLE);
	channel_command(device, CMD_APPLE_CH_TEMPORAL_FILTER_DISABLE);
	channel_command(device, CMD_APPLE_CH_MOTION_HISTORY_STOP);
	channel_command(device, CMD_APPLE_CH_TEMPORAL_FILTER_STOP);
}


status_t
fthd_stream_start(fthd_device* device)
{
	MutexLocker locker(device->stream_lock);
	if (device->streaming)
		return B_BUSY;

	// The camera is switched off while unused (see fthd_power_down()).
	status_t powerStatus = fthd_power_up(device);
	if (powerStatus != B_OK)
		return powerStatus;
	device->last_used = system_time();

	device->frame_size = (device->width * device->height * 2 + B_PAGE_SIZE
		- 1) & ~(size_t)(B_PAGE_SIZE - 1);
	status_t status = map_frames(device);
	if (status != B_OK) {
		FTHD_LOG("setting up frame buffers failed: %s\n", strerror(status));
		unmap_frames(device);
		return status;
	}

	// forget frames of an earlier stream
	int32 count;
	if (get_sem_count(device->frame_sem, &count) == B_OK && count > 0)
		acquire_sem_etc(device->frame_sem, count, B_RELATIVE_TIMEOUT, 0);
	device->sequence = 0;
	device->frames_received = 0;
	device->stream_start = system_time();

	status = start_channel(device);
	if (status != B_OK) {
		FTHD_LOG("starting the stream failed: %s\n", strerror(status));
		stop_channel(device);
		unmap_frames(device);
		return status;
	}
	device->streaming = true;

	int32 queued = 0;
	for (int32 i = 0; i < FTHD_FRAME_BUFFERS; i++) {
		if (queue_frame(device, i) == B_OK)
			queued++;
	}
	FTHD_LOG("stream started, %" B_PRId32 " buffers queued\n", queued);
	return queued > 0 ? B_OK : B_ERROR;
}


void
fthd_stream_stop(fthd_device* device)
{
	MutexLocker locker(device->stream_lock);
	if (!device->streaming)
		return;

	stop_channel(device);

	// the buffer return command gives back what the camera still holds
	bool held = true;
	for (int32 i = 0; i < 100 && held; i++) {
		held = false;
		MutexLocker frameLocker(device->frame_lock);
		for (int32 j = 0; j < FTHD_FRAME_BUFFERS; j++) {
			if (device->frames[j].state == FTHD_FRAME_QUEUED)
				held = true;
		}
		frameLocker.Unlock();
		if (held)
			snooze(10000);
	}

	device->streaming = false;
	device->last_used = system_time();
	bigtime_t duration = system_time() - device->stream_start;
	int32 frames = atomic_get(&device->frames_received);
	FTHD_LOG("stream stopped: %" B_PRId32 " frames in %" B_PRId64 " ms "
		"(%" B_PRId64 ".%" B_PRId64 " fps)\n", frames, duration / 1000,
		duration > 0 ? (int64)frames * 1000000 / duration : 0,
		duration > 0 ? ((int64)frames * 10000000 / duration) % 10 : 0);
	unmap_frames(device);
}


/*!	Waits for a filled frame, copies the newest one to the reader's
	\a buffer and gives every frame it passed over back to the camera.
*/
status_t
fthd_stream_read_frame(fthd_device* device, void* buffer, size_t bufferSize,
	bigtime_t timeout, uint32* _sequence, bigtime_t* _time)
{
	MutexLocker locker(device->stream_lock);
	if (!device->streaming)
		return B_NOT_ALLOWED;

	size_t imageSize = device->width * device->height * 2;
	if (bufferSize < imageSize)
		return B_BUFFER_OVERFLOW;

	int32 newest = -1;
	int32 passed[FTHD_FRAME_BUFFERS];
	int32 passedCount = 0;
	bigtime_t deadline = system_time() + timeout;
	while (newest < 0) {
		status_t status = acquire_sem_etc(device->frame_sem, 1,
			B_ABSOLUTE_TIMEOUT | B_CAN_INTERRUPT, deadline);
		if (status != B_OK)
			return status;

		MutexLocker frameLocker(device->frame_lock);
		for (int32 i = 0; i < FTHD_FRAME_BUFFERS; i++) {
			if (device->frames[i].state != FTHD_FRAME_DONE)
				continue;
			if (newest < 0 || device->frames[i].sequence
					> device->frames[newest].sequence) {
				if (newest >= 0)
					passed[passedCount++] = newest;
				newest = i;
			} else
				passed[passedCount++] = i;
		}
		if (newest >= 0)
			device->frames[newest].state = FTHD_FRAME_READING;
		for (int32 i = 0; i < passedCount; i++)
			device->frames[passed[i]].state = FTHD_FRAME_READING;
	}
	// one count was taken for each frame passed over too
	if (passedCount > 0) {
		acquire_sem_etc(device->frame_sem, passedCount, B_RELATIVE_TIMEOUT,
			0);
	}

	fthd_frame& frame = device->frames[newest];
	status_t status = user_memcpy(buffer,
		device->frame_memory + newest * device->frame_size, imageSize);
	*_sequence = frame.sequence;
	*_time = frame.time;

	queue_frame(device, newest);
	for (int32 i = 0; i < passedCount; i++)
		queue_frame(device, passed[i]);
	return status;
}
