/*
 * Copyright 2026, RenkuOS Contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */
#ifndef FACETIMEHD_IOCTL_H
#define FACETIMEHD_IOCTL_H


//!	The private interface of /dev/video/facetimehd/0. It is not stable yet.


#include <Drivers.h>


#define FTHD_IOCTL_VERSION		2

enum {
	FTHD_GET_INFO = B_DEVICE_OP_CODES_END + 0x4654,
		// fthd_info*
	FTHD_START,
		// no argument; 1280x720 YUYV (B_YCbCr422) at up to 30 fps
	FTHD_STOP,
		// no argument
	FTHD_READ_FRAME,
		// fthd_frame_request*; waits for the newest frame and copies it
	FTHD_GET_CONTROL,
		// fthd_control*
	FTHD_SET_CONTROL
		// fthd_control*; kept for later streams, applied now if streaming
};


// picture controls
enum {
	FTHD_CONTROL_BRIGHTNESS = 0,	// 0-255, default 128
	FTHD_CONTROL_CONTRAST,			// 0-255, default 128
	FTHD_CONTROL_SATURATION,		// 0-255, default 128
	FTHD_CONTROL_HUE,				// 0-255, default 128
	FTHD_CONTROL_AUTO_WHITE_BALANCE,	// 0 or 1, default 1
	FTHD_CONTROL_LOW_LIGHT,			// 0 or 1, default 0: let the frame rate
									// drop to 5 fps for a brighter picture
	FTHD_CONTROL_COUNT
};


// Saved picture settings. The Camera preferences write them to
// ~/config/settings/kernel/drivers/facetimehd in driver settings format,
// one "<name> <value>" line per control, with the driver's values
// (0-255, or 0 and 1); the driver reads them when it loads, so they
// survive a restart. Names in FTHD_CONTROL order.
#define FTHD_SETTINGS_NAME		"facetimehd"
static const char* const kFTHDControlNames[FTHD_CONTROL_COUNT] = {
	"brightness",
	"contrast",
	"saturation",
	"hue",
	"auto_white_balance",
	"low_light"
};


struct fthd_control {
	uint32		id;
	int32		value;
};


struct fthd_info {
	uint32		version;			// FTHD_IOCTL_VERSION
	uint16		sensor_id0;
	uint16		sensor_id1;
	uint32		sensor_count;
	uint32		sensor_width;		// 0 if unknown
	uint32		sensor_height;
	uint32		width;				// of the frames FTHD_READ_FRAME returns
	uint32		height;
	uint32		bytes_per_row;
	uint32		frame_size;
	bool		streaming;
	uint32		frames_received;
	bool		set_file_loaded;	// colour calibration
};


struct fthd_frame_request {
	void*		buffer;
	size_t		buffer_size;		// at least fthd_info::frame_size
	bigtime_t	timeout;			// relative; 0 means one second

	// filled in
	uint32		sequence;			// frame number since FTHD_START
	bigtime_t	time;				// system_time() when it arrived
};


#endif	// FACETIMEHD_IOCTL_H
