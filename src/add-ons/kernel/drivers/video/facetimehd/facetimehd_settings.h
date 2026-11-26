/*
 * Copyright 2026, RenkuOS Contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */
#ifndef FACETIMEHD_SETTINGS_H
#define FACETIMEHD_SETTINGS_H


/*!	Saving the camera settings, for userland programs that change them (the
	Camera preferences and the camera's media add-on, which CodyCam's
	"Video settings" window uses). The driver holds the current values;
	this writes all of them to the driver's settings file (see
	facetimehd_ioctl.h), which the driver reads when it loads.
*/


#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <FindDirectory.h>
#include <StorageDefs.h>

#include "facetimehd_ioctl.h"


// Picture sizes the settings windows offer (the camera makes any even size
// up to 1280x720).
static const struct {
	int32		width;
	int32		height;
	const char*	label;
} kFTHDPictureSizes[] = {
	{ 1280, 720, "1280 \xC3\x97 720 (HD)" },
	{ 960, 540, "960 \xC3\x97 540" },
	{ 640, 480, "640 \xC3\x97 480 (VGA)" },
	{ 640, 360, "640 \xC3\x97 360" },
	{ 320, 240, "320 \xC3\x97 240 (QVGA)" }
};
static const int32 kFTHDPictureSizeCount
	= sizeof(kFTHDPictureSizes) / sizeof(kFTHDPictureSizes[0]);


/*!	Writes every setting of the open camera \a device to
	~/config/settings/kernel/drivers/facetimehd, through a temporary file
	so that a crash never leaves a half-written one behind.
*/
static inline status_t
fthd_save_settings(int device)
{
	int32 values[FTHD_CONTROL_COUNT];
	for (uint32 id = 0; id < FTHD_CONTROL_COUNT; id++) {
		fthd_control control = { id, 0 };
		if (ioctl(device, FTHD_GET_CONTROL, &control, sizeof(control)) != 0)
			return errno;
		values[id] = control.value;
	}

	char directory[B_PATH_NAME_LENGTH];
	status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, -1, true,
		directory, sizeof(directory));
	if (status != B_OK)
		return status;
	strlcat(directory, "/kernel", sizeof(directory));
	mkdir(directory, 0755);
	strlcat(directory, "/drivers", sizeof(directory));
	mkdir(directory, 0755);

	char path[B_PATH_NAME_LENGTH];
	char temporary[B_PATH_NAME_LENGTH];
	if (snprintf(path, sizeof(path), "%s/%s", directory, FTHD_SETTINGS_NAME)
			>= (int)sizeof(path)
		|| snprintf(temporary, sizeof(temporary), "%s~", path)
			>= (int)sizeof(temporary))
		return B_NAME_TOO_LONG;

	FILE* file = fopen(temporary, "w");
	if (file == NULL)
		return errno;
	fprintf(file, "# FaceTime HD camera settings, written by the Camera "
		"preferences or\n# the camera's video settings. The facetimehd "
		"driver reads them when it\n# loads.\n");
	for (uint32 id = 0; id < FTHD_CONTROL_COUNT; id++)
		fprintf(file, "%s %" B_PRId32 "\n", kFTHDControlNames[id], values[id]);
	if (fflush(file) != 0 || fsync(fileno(file)) != 0) {
		status = errno;
		fclose(file);
		unlink(temporary);
		return status;
	}
	if (fclose(file) != 0) {
		status = errno;
		unlink(temporary);
		return status;
	}

	if (rename(temporary, path) != 0) {
		status = errno;
		unlink(temporary);
		return status;
	}
	return B_OK;
}


#endif	// FACETIMEHD_SETTINGS_H
