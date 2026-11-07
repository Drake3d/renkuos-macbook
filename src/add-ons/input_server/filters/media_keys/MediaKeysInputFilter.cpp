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
#include <Screen.h>

#include "MixerControl.h"


// A consumer control usage arrives as this plus the usage, as the shared HID
// keyboard code reports it for USB keyboards and the Apple SPI keyboard.
static const int32 kConsumerKeyBase = 0x000c0000;
static const int32 kBrightnessUpKey = kConsumerKeyBase + 0x6f;
static const int32 kBrightnessDownKey = kConsumerKeyBase + 0x70;
static const int32 kMuteKey = kConsumerKeyBase + 0xe2;
static const int32 kVolumeUpKey = kConsumerKeyBase + 0xe9;
static const int32 kVolumeDownKey = kConsumerKeyBase + 0xea;

// dB per key press; a held key repeats.
static const float kVolumeStep = 3.0f;
// The screen brightness goes from off to full in this many key presses.
// The graphics driver keeps the backlight a little on at the lowest one.
static const int32 kBrightnessSteps = 16;

enum {
	kMsgVolumeUp = 'mkVU',
	kMsgVolumeDown = 'mkVD',
	kMsgToggleMute = 'mkMT',
	kMsgBrightnessUp = 'mkBU',
	kMsgBrightnessDown = 'mkBD'
};


//!	Talks to the media server and the app server in a thread of its own,
//	so that a slow or missing media server never holds up the input.
class MediaKeysLooper : public BLooper {
public:
	MediaKeysLooper()
		:
		BLooper("media keys"),
		fMixer(NULL)
	{
	}

	virtual ~MediaKeysLooper()
	{
		delete fMixer;
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case kMsgVolumeUp:
			case kMsgVolumeDown:
			case kMsgToggleMute:
				_ChangeVolume(message->what);
				break;
			case kMsgBrightnessUp:
			case kMsgBrightnessDown:
				_ChangeBrightness(message->what == kMsgBrightnessUp ? 1 : -1);
				break;
			default:
				BLooper::MessageReceived(message);
				break;
		}
	}

private:
	void _ChangeVolume(uint32 command)
	{
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

		if (command == kMsgToggleMute) {
			fMixer->SetMuted(!fMixer->IsMuted());
			return;
		}

		fMixer->SetMuted(false);
		fMixer->ChangeVolumeBy(command == kMsgVolumeUp
			? kVolumeStep : -kVolumeStep);
	}

	void _ChangeBrightness(int32 steps)
	{
		// The app server remembers the brightness for the next boot, as it
		// does when it is set in the Screen preferences.
		BScreen screen;
		float brightness;
		if (screen.GetBrightness(&brightness) != B_OK)
			return;

		// Snap to the nearest step, in case it was set elsewhere.
		int32 step = (int32)(brightness * kBrightnessSteps + 0.5f) + steps;
		if (step < 0)
			step = 0;
		else if (step > kBrightnessSteps)
			step = kBrightnessSteps;

		screen.SetBrightness((float)step / kBrightnessSteps);
	}

	MixerControl*	fMixer;
};


extern "C" BInputServerFilter*
instantiate_input_filter()
{
	return new(std::nothrow) MediaKeysInputFilter();
}


MediaKeysInputFilter::MediaKeysInputFilter()
	:
	fLooper(new(std::nothrow) MediaKeysLooper()),
	fMuteHeld(false)
{
	if (fLooper != NULL)
		fLooper->Run();
}


MediaKeysInputFilter::~MediaKeysInputFilter()
{
	if (fLooper != NULL && fLooper->Lock())
		fLooper->Quit();
}


status_t
MediaKeysInputFilter::InitCheck()
{
	return fLooper != NULL ? B_OK : B_NO_MEMORY;
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
		case kBrightnessUpKey:
			command = kMsgBrightnessUp;
			break;
		case kBrightnessDownKey:
			command = kMsgBrightnessDown;
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
		fLooper->PostMessage(command);

	return B_SKIP_MESSAGE;
}
