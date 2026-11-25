/*
 * Copyright 2026, RenkuOS Contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */
#ifndef FACETIMEHD_ADD_ON_H
#define FACETIMEHD_ADD_ON_H


#include <MediaAddOn.h>
#include <MediaFormats.h>


class FaceTimeHDAddOn : public BMediaAddOn {
public:
								FaceTimeHDAddOn(image_id image);
	virtual						~FaceTimeHDAddOn();

	virtual	status_t			InitCheck(const char** _failureText);
	virtual	int32				CountFlavors();
	virtual	status_t			GetFlavorAt(int32 index,
									const flavor_info** _info);
	virtual	BMediaNode*			InstantiateNodeFor(const flavor_info* info,
									BMessage* config, status_t* _error);

	virtual	status_t			GetConfigurationFor(BMediaNode* node,
									BMessage* message);
	virtual	bool				WantsAutoStart();
	virtual	status_t			AutoStart(int index, BMediaNode** _node,
									int32* _internalID, bool* _hasMore);

private:
			status_t			fInitStatus;
			flavor_info			fFlavorInfo;
			media_format		fFormat;
};


#endif	// FACETIMEHD_ADD_ON_H
