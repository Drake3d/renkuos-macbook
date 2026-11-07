/*
 * Copyright 2026, RenkuOS Contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


#include "MediaKeysInputFilter.h"

#include <new>

#include <InterfaceDefs.h>
#include <Looper.h>
#include <Message.h>

#include "MixerControl.h"


// A consumer control usage arrives as this plus the usage, as the shared HID
// keyboard code reports it for USB keyboards and the Apple SPI keyboard.
static const int32 kConsumerKeyBase = 0x000c0000;
static const int32 kMuteKey = kConsumerKeyBase + 0xe2;
static const int32 kVolumeUpKey = kConsumerKeyBase + 0xe9;
static const int32 kVolumeDownKey = kConsumerKeyBase + 0xea;

// dB per key press; a held key repeats.
static const float kVolumeStep = 3.0f;

enum {
	kMsgVolumeUp = 'mkVU',
	kMsgVolumeDown = 'mkVD',
	kMsgToggleMute = 'mkMT'
};


//!	Talks to the media server in a thread of its own, so that a slow or
//	missing media server never holds up the input.
class VolumeLooper : public BLooper {
public:
	VolumeLooper()
		:
		BLooper("media keys"),
		fMixer(NULL)
	{
	}

	virtual ~VolumeLooper()
	{
		delete fMixer;
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case kMsgVolumeUp:
			case kMsgVolumeDown:
			case kMsgToggleMute:
				break;
			default:
				BLooper::MessageReceived(message);
				return;
		}

		// Created here rather than at start-up, since the media server may
		// not be running yet then.
		if (fMixer == NULL) {
			fMixer = new(std::nothrow) MixerControl();
			if (fMixer == NULL)
				return;
		}

		// Connect anew each time: the media server may have been restarted
		// and the Deskbar setting for which volume to use may have changed.
		if (!fMixer->Connect())
			return;

		if (message->what == kMsgToggleMute) {
			fMixer->SetMuted(!fMixer->IsMuted());
			return;
		}

		fMixer->SetMuted(false);
		fMixer->ChangeVolumeBy(message->what == kMsgVolumeUp
			? kVolumeStep : -kVolumeStep);
	}

private:
	MixerControl*	fMixer;
};


extern "C" BInputServerFilter*
instantiate_input_filter()
{
	return new(std::nothrow) MediaKeysInputFilter();
}


MediaKeysInputFilter::MediaKeysInputFilter()
	:
	fVolumeLooper(new(std::nothrow) VolumeLooper()),
	fMuteHeld(false)
{
	if (fVolumeLooper != NULL)
		fVolumeLooper->Run();
}


MediaKeysInputFilter::~MediaKeysInputFilter()
{
	if (fVolumeLooper != NULL && fVolumeLooper->Lock())
		fVolumeLooper->Quit();
}


status_t
MediaKeysInputFilter::InitCheck()
{
	return fVolumeLooper != NULL ? B_OK : B_NO_MEMORY;
}


filter_result
MediaKeysInputFilter::Filter(BMessage* message, BList* _list)
{
	if (message->what != B_UNMAPPED_KEY_DOWN
		&& message->what != B_UNMAPPED_KEY_UP)
		return B_DISPATCH_MESSAGE;

	int32 key;
	if (message->FindInt32("key", &key) != B_OK)
		return B_DISPATCH_MESSAGE;

	bool down = message->what == B_UNMAPPED_KEY_DOWN;
	uint32 command;
	switch (key) {
		case kVolumeUpKey:
			command = kMsgVolumeUp;
			break;
		case kVolumeDownKey:
			command = kMsgVolumeDown;
			break;
		case kMuteKey:
		{
			// A held key repeats; mute only toggles once per press.
			bool wasHeld = fMuteHeld;
			fMuteHeld = down;
			if (wasHeld)
				return B_SKIP_MESSAGE;
			command = kMsgToggleMute;
			break;
		}
		default:
			return B_DISPATCH_MESSAGE;
	}

	if (down)
		fVolumeLooper->PostMessage(command);

	return B_SKIP_MESSAGE;
}
