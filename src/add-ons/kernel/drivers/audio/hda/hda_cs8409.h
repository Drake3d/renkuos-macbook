/*
 * Copyright 2026, RenkuOS Contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */
#ifndef HDA_CS8409_H
#define HDA_CS8409_H


#include "driver.h"


//!	Support for the Cirrus Logic CS8409 as Apple uses it: an HDA bridge
//	whose speaker amplifiers sit behind it on its own I2C bus.

bool		hda_cs8409_is_apple(hda_codec* codec);
void		hda_cs8409_init(hda_codec* codec);

// Which widgets the playback stream may use; the others are unused bridge
// outputs and are left alone.
bool		hda_cs8409_use_playback_widget(hda_codec* codec, uint32 nodeID);
// The playback stream runs with this many channels whatever the user side
// asks for; 0 for no restriction.
uint32		hda_cs8409_playback_channels(hda_codec* codec);

void		hda_cs8409_playback_start(hda_audio_group* audioGroup);
// Called with every buffer exchange while playing.
void		hda_cs8409_playback_running(hda_audio_group* audioGroup);
void		hda_cs8409_playback_stop(hda_audio_group* audioGroup);


#endif	// HDA_CS8409_H
