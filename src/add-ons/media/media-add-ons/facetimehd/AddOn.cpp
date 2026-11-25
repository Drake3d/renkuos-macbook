/*
 * Copyright 2026, RenkuOS Contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


//!	Media add-on for the FaceTime HD camera (facetimehd driver).


#include "AddOn.h"

#include <errno.h>
#include <fcntl.h>
#include <new>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "Producer.h"


FaceTimeHDAddOn::FaceTimeHDAddOn(image_id image)
	:
	BMediaAddOn(image),
	fInitStatus(B_NO_INIT)
{
	// Opening the device does not turn the camera on; it only shows that
	// the driver found the camera and started its firmware.
	int fd = open(kFaceTimeHDDevice, O_RDONLY);
	if (fd >= 0) {
		close(fd);
		fInitStatus = B_OK;
	} else {
		fInitStatus = B_DEVICE_NOT_FOUND;
		syslog(LOG_INFO, "facetimehd media add-on: %s: %s\n",
			kFaceTimeHDDevice, strerror(errno));
	}

	fFormat.type = B_MEDIA_RAW_VIDEO;
	fFormat.u.raw_video = media_raw_video_format::wildcard;

	fFlavorInfo.name = (char*)"FaceTime HD camera";
	fFlavorInfo.info = (char*)"The built-in camera of the MacBook Pro";
	// a physical input becomes a live node, and the default video input
	fFlavorInfo.kinds = B_BUFFER_PRODUCER | B_CONTROLLABLE | B_PHYSICAL_INPUT;
	fFlavorInfo.flavor_flags = 0;
	fFlavorInfo.internal_id = 0;
	fFlavorInfo.possible_count = 1;
	fFlavorInfo.in_format_count = 0;
	fFlavorInfo.in_format_flags = 0;
	fFlavorInfo.in_formats = NULL;
	fFlavorInfo.out_format_count = 1;
	fFlavorInfo.out_format_flags = 0;
	fFlavorInfo.out_formats = &fFormat;
}


FaceTimeHDAddOn::~FaceTimeHDAddOn()
{
}


status_t
FaceTimeHDAddOn::InitCheck(const char** _failureText)
{
	if (fInitStatus != B_OK && _failureText != NULL)
		*_failureText = "No FaceTime HD camera";
	return fInitStatus;
}


int32
FaceTimeHDAddOn::CountFlavors()
{
	return fInitStatus == B_OK ? 1 : 0;
}


status_t
FaceTimeHDAddOn::GetFlavorAt(int32 index, const flavor_info** _info)
{
	if (fInitStatus != B_OK)
		return fInitStatus;
	if (index != 0)
		return B_BAD_INDEX;

	*_info = &fFlavorInfo;
	return B_OK;
}


BMediaNode*
FaceTimeHDAddOn::InstantiateNodeFor(const flavor_info* info,
	BMessage* config, status_t* _error)
{
	if (fInitStatus != B_OK || info->internal_id != fFlavorInfo.internal_id) {
		if (_error != NULL)
			*_error = B_BAD_VALUE;
		return NULL;
	}

	FaceTimeHDProducer* node = new(std::nothrow) FaceTimeHDProducer(this,
		fFlavorInfo.name, fFlavorInfo.internal_id);
	if (node == NULL) {
		if (_error != NULL)
			*_error = B_NO_MEMORY;
		return NULL;
	}
	if (_error != NULL)
		*_error = B_OK;
	return node;
}


status_t
FaceTimeHDAddOn::GetConfigurationFor(BMediaNode* node, BMessage* message)
{
	return B_OK;
}


bool
FaceTimeHDAddOn::WantsAutoStart()
{
	return false;
}


status_t
FaceTimeHDAddOn::AutoStart(int index, BMediaNode** _node, int32* _internalID,
	bool* _hasMore)
{
	return B_ERROR;
}


extern "C" BMediaAddOn*
make_media_addon(image_id image)
{
	return new FaceTimeHDAddOn(image);
}
