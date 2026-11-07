/*
 * Copyright 2026, RenkuOS Contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */
#ifndef _MEDIA_KEYS_INPUT_FILTER_H
#define _MEDIA_KEYS_INPUT_FILTER_H


#include <InputServerFilter.h>


extern "C" _EXPORT BInputServerFilter* instantiate_input_filter();


class BLooper;


//!	Turns the mute and volume keys into changes of the system volume, the
//	one the Deskbar volume control shows.
class MediaKeysInputFilter : public BInputServerFilter {
public:
								MediaKeysInputFilter();
	virtual						~MediaKeysInputFilter();

	virtual	status_t			InitCheck();
	virtual	filter_result		Filter(BMessage* message, BList* _list);

private:
			BLooper*			fVolumeLooper;
			bool				fMuteHeld;
};


#endif	// _MEDIA_KEYS_INPUT_FILTER_H
