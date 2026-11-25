/*
 * Copyright 2026, RenkuOS Contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


//!	Test tool for the facetimehd driver: streams a number of frames, reports
//!	the frame rate, and saves the last frame as a PPM image.


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
	fprintf(stderr, "usage: facetimehd_capture [-n frames] [output.ppm]\n"
		"Streams frames from the FaceTime HD camera (default 60) and saves\n"
		"the last one (default /boot/home/facetimehd.ppm).\n");
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


int
main(int argc, char** argv)
{
	int frames = 60;
	const char* output = "/boot/home/facetimehd.ppm";

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
			frames = atoi(argv[++i]);
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
	if (ioctl(fd, FTHD_GET_INFO, &info, sizeof(info)) != 0) {
		fprintf(stderr, "getting the camera info failed: %s\n",
			strerror(errno));
		return 1;
	}
	printf("sensor %04x %04x (%" B_PRIu32 "x%" B_PRIu32 "), stream %"
		B_PRIu32 "x%" B_PRIu32 " YUYV\n", info.sensor_id0, info.sensor_id1,
		info.sensor_width, info.sensor_height, info.width, info.height);
	printf("colour calibration (set file): %s\n",
		info.set_file_loaded ? "loaded" : "not loaded");

	uint8* buffer = (uint8*)malloc(info.frame_size);
	if (buffer == NULL)
		return 1;

	if (ioctl(fd, FTHD_START, NULL, 0) != 0) {
		fprintf(stderr, "starting the stream failed: %s\n", strerror(errno));
		return 1;
	}

	bigtime_t first = 0;
	bigtime_t last = 0;
	uint32 firstSequence = 0;
	uint32 lastSequence = 0;
	int received = 0;
	for (int i = 0; i < frames; i++) {
		fthd_frame_request request = {};
		request.buffer = buffer;
		request.buffer_size = info.frame_size;
		request.timeout = 2000000;
		if (ioctl(fd, FTHD_READ_FRAME, &request, sizeof(request)) != 0) {
			fprintf(stderr, "frame %d: %s\n", i, strerror(errno));
			break;
		}
		if (received == 0) {
			first = request.time;
			firstSequence = request.sequence;
			printf("first frame: #%" B_PRIu32 "\n", request.sequence);
		}
		last = request.time;
		lastSequence = request.sequence;
		received++;
	}

	ioctl(fd, FTHD_STOP, NULL, 0);
	close(fd);

	if (received == 0) {
		fprintf(stderr, "no frames\n");
		return 1;
	}

	if (received > 1 && last > first) {
		double seconds = (last - first) / 1000000.0;
		printf("%d frames read, camera delivered %" B_PRIu32 " in %.2f s "
			"(%.1f fps)\n", received, lastSequence - firstSequence, seconds,
			(lastSequence - firstSequence) / seconds);
	}

	// a quick look at the image: all-zero means the camera wrote nothing
	uint64 sum = 0;
	for (uint32 i = 0; i < info.frame_size; i += 2)
		sum += buffer[i];
	printf("average brightness of the last frame: %" B_PRIu64 " (of 255)\n",
		sum / (info.frame_size / 2));

	status_t status = save_ppm(output, buffer, info.width, info.height,
		info.bytes_per_row);
	if (status != B_OK) {
		fprintf(stderr, "saving %s failed: %s\n", output, strerror(status));
		return 1;
	}
	printf("saved %s\n", output);
	return 0;
}
