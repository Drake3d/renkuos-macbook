/*
 * Copyright 2026, RenkuOS Contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


//!	Test tool for the facetimehd driver: streams a number of frames at a
//!	chosen size, reports the frame rate, and saves the last frame as a PPM
//!	image; -p tries a list of sizes.


#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <OS.h>

#include "facetimehd_ioctl.h"


static const char* kDevice = "/dev/video/facetimehd/0";


static void
usage()
{
	fprintf(stderr,
		"usage: facetimehd_capture [-n frames] [-s WIDTHxHEIGHT] [output.ppm]\n"
		"       facetimehd_capture -p [-n frames]\n"
		"Streams frames from the FaceTime HD camera (default 60) and saves\n"
		"the last one (default /boot/home/facetimehd.ppm).\n"
		"  -s  frame size (default: the Camera preferences' picture size;\n"
		"      the camera scales)\n"
		"  -p  probe: try a list of sizes, save /boot/home/facetimehd-WxH.ppm\n"
		"      for each and print a summary\n");
	exit(1);
}


static uint8
clamp(int value)
{
	return value < 0 ? 0 : value > 255 ? 255 : value;
}


static status_t
save_ppm(const char* path, const uint8* yuyv, uint32 width, uint32 height,
	uint32 bytesPerRow)
{
	FILE* file = fopen(path, "wb");
	if (file == NULL)
		return errno;

	fprintf(file, "P6\n%" B_PRIu32 " %" B_PRIu32 "\n255\n", width, height);

	uint8* row = (uint8*)malloc(width * 3);
	if (row == NULL) {
		fclose(file);
		return B_NO_MEMORY;
	}

	// ITU-R BT.601, limited range
	for (uint32 y = 0; y < height; y++) {
		const uint8* source = yuyv + y * bytesPerRow;
		for (uint32 x = 0; x + 1 < width; x += 2) {
			int u = source[x * 2 + 1] - 128;
			int v = source[x * 2 + 3] - 128;
			for (int i = 0; i < 2; i++) {
				int c = (source[x * 2 + i * 2] - 16) * 298;
				uint8* pixel = row + (x + i) * 3;
				pixel[0] = clamp((c + 409 * v + 128) >> 8);
				pixel[1] = clamp((c - 100 * u - 208 * v + 128) >> 8);
				pixel[2] = clamp((c + 516 * u + 128) >> 8);
			}
		}
		fwrite(row, 3, width, file);
	}

	free(row);
	return fclose(file) == 0 ? B_OK : errno;
}


struct capture_result {
	status_t	status;
	int			received;
	double		fps;
	uint64		brightness;
};


static capture_result
capture(int fd, uint32 width, uint32 height, int frames, const char* output)
{
	capture_result result = { B_OK, 0, 0, 0 };

	fthd_size size = { width, height };
	if (ioctl(fd, FTHD_SET_SIZE, &size, sizeof(size)) != 0) {
		result.status = errno;
		fprintf(stderr, "%" B_PRIu32 "x%" B_PRIu32 ": the driver refuses this "
			"size: %s\n", width, height, strerror(errno));
		return result;
	}

	fthd_info info;
	if (ioctl(fd, FTHD_GET_INFO, &info, sizeof(info)) != 0) {
		result.status = errno;
		return result;
	}

	uint8* buffer = (uint8*)malloc(info.frame_size);
	if (buffer == NULL) {
		result.status = B_NO_MEMORY;
		return result;
	}

	if (ioctl(fd, FTHD_START, NULL, 0) != 0) {
		result.status = errno;
		fprintf(stderr, "%" B_PRIu32 "x%" B_PRIu32 ": starting the stream "
			"failed: %s\n", width, height, strerror(errno));
		free(buffer);
		return result;
	}

	bigtime_t first = 0;
	bigtime_t last = 0;
	uint32 firstSequence = 0;
	uint32 lastSequence = 0;
	for (int i = 0; i < frames; i++) {
		fthd_frame_request request = {};
		request.buffer = buffer;
		request.buffer_size = info.frame_size;
		request.timeout = 2000000;
		if (ioctl(fd, FTHD_READ_FRAME, &request, sizeof(request)) != 0) {
			result.status = errno;
			fprintf(stderr, "%" B_PRIu32 "x%" B_PRIu32 ": frame %d: %s\n",
				width, height, i, strerror(errno));
			break;
		}
		if (result.received == 0) {
			first = request.time;
			firstSequence = request.sequence;
		}
		last = request.time;
		lastSequence = request.sequence;
		result.received++;
	}

	ioctl(fd, FTHD_STOP, NULL, 0);

	if (result.received > 1 && last > first) {
		result.fps = (lastSequence - firstSequence)
			/ ((last - first) / 1000000.0);
	}

	if (result.received > 0) {
		// a quick look at the image: all-zero means the camera wrote nothing
		uint64 sum = 0;
		for (uint32 i = 0; i < info.frame_size; i += 2)
			sum += buffer[i];
		result.brightness = sum / (info.frame_size / 2);

		status_t status = save_ppm(output, buffer, info.width, info.height,
			info.bytes_per_row);
		if (status != B_OK) {
			fprintf(stderr, "saving %s failed: %s\n", output,
				strerror(status));
		}
		if (result.status == B_OK)
			result.status = status;
	} else if (result.status == B_OK)
		result.status = B_ERROR;

	free(buffer);
	return result;
}


int
main(int argc, char** argv)
{
	int frames = 60;
	const char* output = "/boot/home/facetimehd.ppm";
	uint32 width = FTHD_DEFAULT_WIDTH;
	uint32 height = FTHD_DEFAULT_HEIGHT;
	bool probe = false;
	bool sizeGiven = false;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
			frames = atoi(argv[++i]);
		else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
			if (sscanf(argv[++i], "%" B_SCNu32 "x%" B_SCNu32, &width,
					&height) != 2)
				usage();
			sizeGiven = true;
		} else if (strcmp(argv[i], "-p") == 0)
			probe = true;
		else if (argv[i][0] == '-')
			usage();
		else
			output = argv[i];
	}
	if (frames < 1)
		usage();

	int fd = open(kDevice, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "%s: %s\n", kDevice, strerror(errno));
		return 1;
	}

	fthd_info info;
	if (ioctl(fd, FTHD_GET_INFO, &info, sizeof(info)) != 0
		|| info.version != FTHD_IOCTL_VERSION) {
		fprintf(stderr, "the camera driver does not match this tool\n");
		return 1;
	}
	printf("sensor %04x %04x (%" B_PRIu32 "x%" B_PRIu32 "), colour "
		"calibration %s\n", info.sensor_id0, info.sensor_id1,
		info.sensor_width, info.sensor_height,
		info.set_file_loaded ? "loaded" : "not loaded");

	// without -s: the default size chosen in the Camera preferences
	if (!sizeGiven) {
		width = info.width;
		height = info.height;
	}

	if (!probe) {
		capture_result result = capture(fd, width, height, frames, output);
		close(fd);
		if (result.status != B_OK)
			return 1;
		printf("%" B_PRIu32 "x%" B_PRIu32 ": %d frames, %.1f fps, average "
			"brightness %" B_PRIu64 " (of 255), saved %s\n", width, height,
			result.received, result.fps, result.brightness, output);
		return 0;
	}

	// common sizes, and some whose width is not a multiple of 16, 8 or 4
	static const uint32 kSizes[][2] = {
		{ 1280, 720 }, { 1024, 576 }, { 960, 540 }, { 800, 600 },
		{ 640, 480 }, { 640, 360 }, { 352, 288 }, { 320, 240 },
		{ 320, 180 }, { 176, 144 }, { 160, 120 }, { 64, 48 },
		{ 1278, 720 }, { 636, 480 }, { 642, 480 }, { 100, 100 }
	};
	printf("\n%-10s %-8s %6s %6s %s\n", "size", "result", "frames", "fps",
		"brightness");
	int failed = 0;
	for (size_t i = 0; i < sizeof(kSizes) / sizeof(kSizes[0]); i++) {
		char path[B_PATH_NAME_LENGTH];
		snprintf(path, sizeof(path), "/boot/home/facetimehd-%" B_PRIu32 "x%"
			B_PRIu32 ".ppm", kSizes[i][0], kSizes[i][1]);
		capture_result result = capture(fd, kSizes[i][0], kSizes[i][1],
			frames, path);
		char size[32];
		snprintf(size, sizeof(size), "%" B_PRIu32 "x%" B_PRIu32, kSizes[i][0],
			kSizes[i][1]);
		printf("%-10s %-8s %6d %6.1f %" B_PRIu64 "\n", size,
			result.status == B_OK ? "ok" : "FAILED", result.received,
			result.fps, result.brightness);
		if (result.status != B_OK)
			failed++;
	}
	close(fd);
	printf("\n%d of %d sizes failed; pictures in /boot/home/facetimehd-*.ppm"
		"\n", failed, (int)(sizeof(kSizes) / sizeof(kSizes[0])));
	return failed == 0 ? 0 : 1;
}
