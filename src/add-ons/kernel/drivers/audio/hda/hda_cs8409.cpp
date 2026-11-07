/*
 * Copyright 2026, RenkuOS Contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


/*!	The Cirrus Logic CS8409 in the MacBookPro14,1 (subsystem 106b:3300).

	The CS8409 is an HDA bridge. Its converters put the HDA stream onto a
	TDM link to four speaker amplifiers, which only play once they have been
	set up over an I2C bus that the CS8409 drives itself. This file does that
	set-up when playback starts and powers the amplifiers down when it stops.
	The headphone jack has its own codec, a CS42L83 on the same bus; while
	playing, the jack is checked and the sound goes either to the speakers
	or to the headphones. The microphones are not handled yet.

	Sources, as noted at each step:
	[M]	Linux patch_cs8409.c and patch_cs8409.h, by Cirrus Logic: the
		vendor coefficients and how the CS8409 runs its I2C bus.
	[D]	The davidjo/snd_hda_macbookpro project: the TDM, amplifier and
		converter sequences for this machine, which that project traced
		from macOS.
	[S]	Linux ssm3515.c: the SSM3515 amplifier register and bit names.
	[L]	Linux include/sound/cs42l42.h and cs42l42.c: the CS42L42 register
		names and timings; the CS42L83 shares its register map.
	All four are GPL; only facts were taken from them, no code. The
	SSM3515 and CS42L42 datasheets themselves have not been checked yet.
*/


#include "hda_cs8409.h"

#include <stdlib.h>

#include <driver_settings.h>
#include <lock.h>
#include <util/AutoLock.h>

#include "hda_codec_defs.h"


#define TRACE_CS8409
#ifdef TRACE_CS8409
#	define TRACE(a...) dprintf("hda: cs8409: " a)
#else
#	define TRACE(a...)
#endif
#define ERROR(a...) dprintf("hda: cs8409: " a)


#define CIRRUS_VENDOR_ID			0x1013
#define CS8409_PRODUCT_ID			0x8409
#define APPLE_MBP14_1_SUBSYSTEM		0x106b3300

// nodes
#define CS8409_NODE_SPEAKERS_A		0x02	// converter to pin 0x24
#define CS8409_NODE_SPEAKERS_B		0x03	// converter to pin 0x25
#define CS8409_PIN_SPEAKERS_A		0x24
#define CS8409_PIN_SPEAKERS_B		0x25
#define CS8409_NODE_VENDOR			0x47	// coefficients, I2C

// vendor coefficients [M]
#define CS8409_DEV_CFG1				0x00
#define CS8409_DEV_CFG1_I2C_CLOCK	0x0008
#define CS8409_DEV_CFG3				0x02
#define CS8409_DEV_CFG3_I2C_MODE	0x0080
#define CS8409_I2C_ADDRESS			0x59
#define CS8409_I2C_CONTROL			0x5b
#define CS8409_I2C_CONTROL_100KHZ	0x0200
#define CS8409_I2C_STATUS			0x5c
#define CS8409_I2C_STATUS_DONE		0x0018
#define CS8409_I2C_WRITE			0x5d
#define CS8409_I2C_READ				0x5e

// vendor verb taking an 8 bit payload, sent after TDM changes [D]
#define CS8409_VID_TDM_COMMIT		0x7f000

// The amplifiers: 8 bit I2C address and TDM slot of each [D].
static const struct {
	uint8	address;
	uint8	slot;
} kAmplifiers[] = {
	{ 0x28, 0 },
	{ 0x2a, 2 },
	{ 0x2c, 1 },
	{ 0x2e, 3 },
};

// SSM3515 amplifier registers, named as in the Linux ssm3515 driver [S]
#define AMP_POWER					0x00	// bit 0: software power-down
#define AMP_GAIN_EDGE				0x01
#define AMP_DAC_CONTROL				0x02	// bit 6: mute, 2-0: rate range
#define AMP_DAC_VOLUME				0x03	// 0.375 dB steps, higher is louder
#define AMP_SAI_CONTROL1			0x04
#define AMP_SAI_CONTROL2			0x05	// TDM slot
#define AMP_STATUS					0x0a	// fault and warning bits

// The volume the amplifiers run at by default: 0x40 is 0 dB and each step up
// is 0.375 dB quieter, so 0x60 is -12 dB; macOS uses 0x48, -3 dB [D].
// "apple_amp_volume <value>" in the hda.settings driver settings changes it.
// Haiku's mixer volume applies on top.
static const uint8 kDefaultAmplifierVolume = 0x60;
static uint8 sAmplifierVolume = kDefaultAmplifierVolume;

// The second speaker pair's converter takes channels 2 and 3 of the four
// channel stream: with both converters on channels 0 and 1, neither pair
// plays. The media side fills channels 0 and 1 only, so the driver copies
// them to 2 and 3.
#define CS8409_SPEAKERS_B_CHANNEL	2

// "apple_test_tone true" in hda.settings plays a steady tone on the second
// speaker pair instead, to tell the speaker set-up apart from the media
// side. A triangle wave of 128 frames (344.5 Hz at 44.1 kHz), at -12 dB of
// full scale.
static bool sTestTone = false;
static const uint32 kTestTonePeriod = 128;
static const int32 kTestToneAmplitude = 0x20000000;

// "apple_debug true" in hda.settings logs the state at these buffer
// exchanges while playing (about one and thirty seconds in), and the level
// of the audio every few seconds.
static bool sDebug = false;
static const uint32 kDumpAtExchange[] = { 100, 3000 };
static const uint32 kLevelEveryExchanges = 500;
static const uint32 kLevelLogs = 12;
static uint32 sExchanges;
static uint32 sLevelLogs;

// The headphone jack: a CS42L83 codec on the same I2C bus, fed from the
// CS8409's second serial port through converter 0x0a [D]. Its registers
// are 16 bit, the page in the high byte, and are named here as in the
// CS42L42 [L], which it shares its register map with.
#define CS8409_NODE_HEADPHONES		0x0a	// converter to pin 0x2c
#define CS8409_PIN_HEADPHONES		0x2c
#define CS42L83_ADDRESS				0x90
#define CS42L83_GPIO_RESET			0x02	// on the function group, active low
#define CS42L83_PAGE				0x00	// page select, in every page
#define CS42L83_DEVID_AB			0x1001
#define CS42L83_REVID				0x1005
#define CS42L83_PWR_CTL1			0x1101
#define CS42L83_SRCPL_INT_STATUS	0x130b
#define CS42L83_SRCPL_DAC_LOCKED	0x04
#define CS42L83_TIPSENSE_CTL		0x1b73
#define CS42L83_DET_STATUS1			0x1b77
#define CS42L83_DET_TIP_SENSE		0x80	// a plug is in
#define CS42L83_HP_CTL				0x2001
#define CS42L83_HP_MUTE				0x0c	// both analog mutes
#define CS42L83_MIXER_CHA_VOL		0x2301
#define CS42L83_MIXER_CHB_VOL		0x2303

// Headphone volume, as attenuation in dB of the CS42L83 mixer (0 is
// 0 dB, 63 mutes) [L]. "apple_headphone_volume <dB>" in hda.settings
// changes it.
static const uint8 kDefaultHeadphoneAttenuation = 12;
static uint8 sHeadphoneAttenuation = kDefaultHeadphoneAttenuation;

// The output in use: speakers or headphones, never both. While playing,
// the jack is checked this often and an output change is taken once two
// checks in a row agree.
enum output_type {
	OUTPUT_NONE,
	OUTPUT_SPEAKERS,
	OUTPUT_HEADPHONES
};
static output_type sOutput = OUTPUT_NONE;
static const uint32 kJackCheckEveryExchanges = 10;
static uint32 sJackExchanges;
static bool sLastJackReading;
static bool sHeadphoneCodec;	// the CS42L83 answered at boot

static const int kI2CPolls = 10;
static const bigtime_t kI2CPollDelay = 2000;

static mutex sLock = MUTEX_INITIALIZER("hda cs8409");
static int32 sCodecPage = -1;


bool
hda_cs8409_is_apple(hda_codec* codec)
{
	return codec != NULL && codec->vendor_id == CIRRUS_VENDOR_ID
		&& codec->product_id == CS8409_PRODUCT_ID
		&& codec->subsystem_id == APPLE_MBP14_1_SUBSYSTEM;
}


//	#pragma mark - verbs and coefficients


static void
send_verb(hda_codec* codec, uint32 nodeID, uint32 verbID, uint32 payload)
{
	corb_t verb = MAKE_VERB(codec->addr, nodeID, verbID, payload);
	hda_send_verbs(codec, &verb, NULL, 1);
}


static uint32
read_verb(hda_codec* codec, uint32 nodeID, uint32 verbID)
{
	corb_t verb = MAKE_VERB(codec->addr, nodeID, verbID, 0);
	uint32 response = 0;
	hda_send_verbs(codec, &verb, &response, 1);
	return response;
}


static uint16
read_coef(hda_codec* codec, uint16 index)
{
	corb_t verbs[2];
	verbs[0] = MAKE_VERB(codec->addr, CS8409_NODE_VENDOR,
		VID_SET_COEFFICIENT_INDEX, index);
	verbs[1] = MAKE_VERB(codec->addr, CS8409_NODE_VENDOR,
		VID_GET_PROCESSING_COEFFICIENT, 0);
	uint32 responses[2] = { 0, 0 };
	hda_send_verbs(codec, verbs, responses, 2);
	return responses[1] & 0xffff;
}


static void
write_coef(hda_codec* codec, uint16 index, uint16 value)
{
	corb_t verbs[2];
	verbs[0] = MAKE_VERB(codec->addr, CS8409_NODE_VENDOR,
		VID_SET_COEFFICIENT_INDEX, index);
	verbs[1] = MAKE_VERB(codec->addr, CS8409_NODE_VENDOR,
		VID_SET_PROCESSING_COEFFICIENT, value);
	uint32 responses[2];
	hda_send_verbs(codec, verbs, responses, 2);
}


static void
update_coef(hda_codec* codec, uint16 index, uint16 mask, uint16 value)
{
	uint16 old = read_coef(codec, index);
	write_coef(codec, index, (old & ~mask) | (value & mask));
}


//	#pragma mark - I2C [M]


static status_t
i2c_wait(hda_codec* codec)
{
	for (int i = 0; i < kI2CPolls; i++) {
		snooze(kI2CPollDelay);
		if ((read_coef(codec, CS8409_I2C_STATUS) & CS8409_I2C_STATUS_DONE)
				!= 0) {
			return B_OK;
		}
	}
	return B_TIMED_OUT;
}


static void
i2c_begin(hda_codec* codec, uint8 address)
{
	update_coef(codec, CS8409_DEV_CFG1, CS8409_DEV_CFG1_I2C_CLOCK,
		CS8409_DEV_CFG1_I2C_CLOCK);
	write_coef(codec, CS8409_I2C_ADDRESS, address);
}


static void
i2c_end(hda_codec* codec)
{
	update_coef(codec, CS8409_DEV_CFG1, CS8409_DEV_CFG1_I2C_CLOCK, 0);
}


//!	Writes an 8 bit register of a device without pages.
static status_t
i2c_write(hda_codec* codec, uint8 address, uint8 reg, uint8 value)
{
	i2c_begin(codec, address);
	write_coef(codec, CS8409_I2C_WRITE, (reg << 8) | value);
	status_t status = i2c_wait(codec);
	i2c_end(codec);

	if (status != B_OK) {
		ERROR("I2C 0x%02x: write 0x%02x <- 0x%02x timed out\n", address, reg,
			value);
	} else if (sDebug) {
		TRACE("I2C 0x%02x: 0x%02x <- 0x%02x\n", address, reg, value);
	}
	return status;
}


static status_t
i2c_read(hda_codec* codec, uint8 address, uint8 reg, uint8* _value)
{
	i2c_begin(codec, address);
	write_coef(codec, CS8409_I2C_READ, reg << 8);
	status_t status = i2c_wait(codec);
	if (status == B_OK)
		*_value = read_coef(codec, CS8409_I2C_READ) & 0xff;
	i2c_end(codec);

	if (status != B_OK)
		ERROR("I2C 0x%02x: read 0x%02x timed out\n", address, reg);
	return status;
}


//	#pragma mark - amplifiers


static void
amplifier_power_down(hda_codec* codec, uint8 address)
{
	i2c_write(codec, address, AMP_POWER, 0x01);
}


/*!	The sequence [D] traced from macOS, in the same order except that the
	amplifier is powered up last, once it is set up.
*/
static void
amplifier_setup(hda_codec* codec, uint8 address, uint8 slot)
{
	i2c_write(codec, address, AMP_SAI_CONTROL2, slot);
	i2c_write(codec, address, AMP_GAIN_EDGE, 0x11);
	i2c_write(codec, address, AMP_DAC_VOLUME, sAmplifierVolume);
	i2c_write(codec, address, AMP_SAI_CONTROL1, 0x51);
		// TDM, 24 bit clocks per slot
	i2c_write(codec, address, AMP_DAC_CONTROL, 0x32);
		// 32-48 kHz
	i2c_write(codec, address, AMP_POWER, 0x00);
}


//	#pragma mark - vendor set-up


/*!	The vendor coefficients at boot [D]: the PLL on (it clocks the TDM
	link), every TDM slot off until set up, and the I2C bus enabled.
*/
static void
vendor_init(hda_codec* codec)
{
	TRACE("coefficients before set-up: 0x00 0x%04x, 0x01 0x%04x, "
		"0x02 0x%04x, 0x5b 0x%04x\n", read_coef(codec, 0x00),
		read_coef(codec, 0x01), read_coef(codec, 0x02),
		read_coef(codec, CS8409_I2C_CONTROL));

	write_coef(codec, CS8409_DEV_CFG1, 0x9008);
		// PLL1 enable (0x1000), I2C enable (0x0008)
	write_coef(codec, 0x01, 0x0000);
	write_coef(codec, 0x17, 0x0000);
	write_coef(codec, 0x18, 0x0000);
	write_coef(codec, CS8409_DEV_CFG3, 0x0000);
	for (uint16 index = 0x19; index <= 0x57; index++)
		write_coef(codec, index, 0x8000);
			// ASP transmit and receive slots off
	write_coef(codec, 0x0a, 0x0000);
	write_coef(codec, 0x82, 0x0000);
	write_coef(codec, 0x6b, 0x001f);
	write_coef(codec, 0x6c, 0x001f);

	// I2C mode, 100 kHz [M]; I2C clock off between transfers
	write_coef(codec, CS8409_DEV_CFG3, CS8409_DEV_CFG3_I2C_MODE);
	write_coef(codec, CS8409_I2C_CONTROL, CS8409_I2C_CONTROL_100KHZ);
	write_coef(codec, CS8409_DEV_CFG1, 0x9000);
}


//!	Logs the state of the bridge and the amplifiers, to see why nothing plays.
static void
dump_state(hda_codec* codec)
{
	static const uint16 kCoefs[] = {
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x17, 0x18, 0x19, 0x1a, 0x1b,
		0x1c, 0x6b, 0x71, 0x82
	};
	for (size_t i = 0; i < B_COUNT_OF(kCoefs); i++)
		TRACE("state: coef 0x%02x = 0x%04x\n", kCoefs[i],
			read_coef(codec, kCoefs[i]));

	static const uint32 kNodes[] = {
		CS8409_NODE_SPEAKERS_A, CS8409_NODE_SPEAKERS_B
	};
	for (size_t i = 0; i < B_COUNT_OF(kNodes); i++) {
		TRACE("state: node 0x%02" B_PRIx32 " format 0x%04" B_PRIx32
			", stream/channel 0x%02" B_PRIx32 "\n", kNodes[i],
			read_verb(codec, kNodes[i], VID_GET_CONVERTER_FORMAT) & 0xffff,
			read_verb(codec, kNodes[i], VID_GET_CONVERTER_STREAM_CHANNEL)
				& 0xff);
	}
	uint32 group = codec->audio_groups[0]->widget.node_id;
	TRACE("state: function group power 0x%02" B_PRIx32 ", GPIO data 0x%02"
		B_PRIx32 ", mask 0x%02" B_PRIx32 ", direction 0x%02" B_PRIx32 "\n",
		read_verb(codec, group, VID_GET_POWER_STATE) & 0xff,
		read_verb(codec, group, VID_GET_GPIO_DATA) & 0xff,
		read_verb(codec, group, VID_GET_GPIO_EN) & 0xff,
		read_verb(codec, group, VID_GET_GPIO_DIR) & 0xff);
	TRACE("state: coef 0x6c = 0x%04x\n", read_coef(codec, 0x6c));
	TRACE("state: pin 0x24 control 0x%02" B_PRIx32 ", pin 0x25 control 0x%02"
		B_PRIx32 "\n",
		read_verb(codec, CS8409_PIN_SPEAKERS_A, VID_GET_PIN_WIDGET_CONTROL)
			& 0xff,
		read_verb(codec, CS8409_PIN_SPEAKERS_B, VID_GET_PIN_WIDGET_CONTROL)
			& 0xff);

	for (size_t i = 0; i < B_COUNT_OF(kAmplifiers); i++) {
		uint8 address = kAmplifiers[i].address;
		uint8 power = 0, dac = 0, volume = 0, status = 0;
		i2c_read(codec, address, AMP_POWER, &power);
		i2c_read(codec, address, AMP_DAC_CONTROL, &dac);
		i2c_read(codec, address, AMP_DAC_VOLUME, &volume);
		i2c_read(codec, address, AMP_STATUS, &status);
		TRACE("state: amplifier 0x%02x power 0x%02x, DAC 0x%02x, volume "
			"0x%02x, status 0x%02x\n", address, power, dac, volume, status);
	}
}


//	#pragma mark - TDM link [D]


static void
tdm_enable(hda_codec* codec)
{
	// sample rate fields, for 44.1 kHz
	update_coef(codec, 0x01, 0x0380, 0x0200);
	update_coef(codec, 0x05, 0x0007, 0x0001);
	update_coef(codec, 0x04, 0x01ff, 0x08ff);

	// slots of the first converter's two channels
	write_coef(codec, 0x19, 0x0800);
	write_coef(codec, 0x1a, 0x0820);

	uint16 config = read_coef(codec, CS8409_DEV_CFG1);
	write_coef(codec, CS8409_DEV_CFG1, 0xb000);
	write_coef(codec, 0x04, 0x08ff);
	write_coef(codec, CS8409_DEV_CFG1, config);

	write_coef(codec, 0x03, 0x8000);
	write_coef(codec, 0x05, 0x0001);
	update_coef(codec, 0x82, 0x5400, 0x5400);
	write_coef(codec, CS8409_DEV_CFG3, 0x0280);
	update_coef(codec, 0x01, 0x0020, 0x0020);
	write_coef(codec, 0x6b, 0x001f);
	update_coef(codec, 0x71, 0x400f, 0x400f);
	send_verb(codec, CS8409_NODE_VENDOR, CS8409_VID_TDM_COMMIT, 0xb6);
	send_verb(codec, CS8409_PIN_SPEAKERS_A, VID_SET_PIN_WIDGET_CONTROL,
		PIN_ENABLE_OUTPUT);

	// slots of the second converter's two channels
	write_coef(codec, 0x1b, 0x0840);
	write_coef(codec, 0x1c, 0x0860);
	write_coef(codec, 0x6b, 0x001f);
	update_coef(codec, 0x71, 0x400f, 0x400f);
	send_verb(codec, CS8409_NODE_VENDOR, CS8409_VID_TDM_COMMIT, 0xb6);
	send_verb(codec, CS8409_PIN_SPEAKERS_B, VID_SET_PIN_WIDGET_CONTROL,
		PIN_ENABLE_OUTPUT);
}


static void
tdm_disable(hda_codec* codec)
{
	write_coef(codec, 0x19, 0x8800);
	write_coef(codec, 0x1a, 0x8820);
	write_coef(codec, 0x6b, 0x001f);
	send_verb(codec, CS8409_NODE_VENDOR, CS8409_VID_TDM_COMMIT, 0xb6);
	send_verb(codec, CS8409_PIN_SPEAKERS_A, VID_SET_PIN_WIDGET_CONTROL, 0);

	write_coef(codec, 0x1b, 0x8840);
	write_coef(codec, 0x1c, 0x8860);
	update_coef(codec, 0x82, 0x5400, 0);
	update_coef(codec, 0x01, 0x0020, 0);
	write_coef(codec, 0x71, 0);
	send_verb(codec, CS8409_NODE_VENDOR, CS8409_VID_TDM_COMMIT, 0);
	send_verb(codec, CS8409_PIN_SPEAKERS_B, VID_SET_PIN_WIDGET_CONTROL, 0);
}


/*!	Starts (or stops) both converters on the TDM link together: each is taken
	off its stream while coefficient 0x17 changes, then put back [D].
*/
static void
sync_converters(hda_codec* codec, bool start)
{
	uint32 streamA = read_verb(codec, CS8409_NODE_SPEAKERS_A,
		VID_GET_CONVERTER_STREAM_CHANNEL) & 0xff;
	uint32 streamB = read_verb(codec, CS8409_NODE_SPEAKERS_B,
		VID_GET_CONVERTER_STREAM_CHANNEL) & 0xff;

	send_verb(codec, CS8409_NODE_SPEAKERS_A, VID_SET_CONVERTER_STREAM_CHANNEL,
		0);
	write_coef(codec, 0x17, start ? 0x0001 : 0x0002);
	send_verb(codec, CS8409_NODE_SPEAKERS_B, VID_SET_CONVERTER_STREAM_CHANNEL,
		0);
	write_coef(codec, 0x17, start ? 0x0003 : 0x0000);
	if (!start)
		write_coef(codec, 0x01, 0x0220);

	send_verb(codec, CS8409_NODE_SPEAKERS_A, VID_SET_CONVERTER_STREAM_CHANNEL,
		streamA);
	send_verb(codec, CS8409_NODE_SPEAKERS_B, VID_SET_CONVERTER_STREAM_CHANNEL,
		streamB);
}


/*!	Copies channels 0 and 1 of every playback buffer to channels 2 and 3, for
	the second speaker pair. Called from the media server's thread while it
	waits in buffer_exchange, so it does not write the buffers at the same
	time; the buffers are mapped into it, so they are only accessed through
	user_memcpy().
*/
static void
copy_to_second_pair(hda_stream* stream)
{
	if (stream->sample_size != 4 || stream->num_channels != 4)
		return;

	int32 chunk[256];
	uint32 framesPerChunk = B_COUNT_OF(chunk) / 4;
	for (uint32 i = 0; i < stream->num_buffers; i++) {
		int32* buffer = (int32*)stream->buffers[i];
		for (uint32 frame = 0; frame < stream->buffer_length;
				frame += framesPerChunk) {
			uint32 frames = min_c(framesPerChunk,
				stream->buffer_length - frame);
			size_t size = frames * 4 * sizeof(int32);
			if (user_memcpy(chunk, buffer + frame * 4, size) != B_OK)
				return;
			for (uint32 j = 0; j < frames; j++) {
				chunk[j * 4 + 2] = chunk[j * 4];
				chunk[j * 4 + 3] = chunk[j * 4 + 1];
			}
			if (user_memcpy(buffer + frame * 4, chunk, size) != B_OK)
				return;
		}
	}
}


/*!	Writes the test tone into channels 2 and 3 of every playback buffer. The
	buffers are mapped into the media server too, so they are written through
	user_memcpy().
*/
static void
start_test_tone(hda_stream* stream)
{
	if (stream->sample_size != 4 || stream->num_channels < 4)
		return;

	uint32 frameIndex = 0;
	for (uint32 i = 0; i < stream->num_buffers; i++) {
		int32* buffer = (int32*)stream->buffers[i];
		for (uint32 frame = 0; frame < stream->buffer_length;
				frame++, frameIndex++) {
			uint32 phase = frameIndex % kTestTonePeriod;
			uint32 quarter = kTestTonePeriod / 4;
			// -quarter..quarter, rising then falling
			int32 level = phase < 2 * quarter ? (int32)phase - (int32)quarter
				: 3 * (int32)quarter - (int32)phase;
			int32 sample = kTestToneAmplitude / (int32)quarter * level;
			int32 pair[2] = { sample, sample };
			if (user_memcpy(buffer + frame * stream->num_channels + 2, pair,
					sizeof(pair)) != B_OK) {
				ERROR("could not write the test tone\n");
				return;
			}
		}
	}

	TRACE("test tone on channels 2 and 3\n");
}


//	#pragma mark - headphone codec (CS42L83)


static status_t
codec_select_page(hda_codec* codec, uint16 reg)
{
	int32 page = reg >> 8;
	if (page == sCodecPage)
		return B_OK;

	status_t status = i2c_write(codec, CS42L83_ADDRESS, CS42L83_PAGE, page);
	sCodecPage = status == B_OK ? page : -1;
	return status;
}


static status_t
codec_write(hda_codec* codec, uint16 reg, uint8 value)
{
	status_t status = codec_select_page(codec, reg);
	if (status != B_OK)
		return status;
	return i2c_write(codec, CS42L83_ADDRESS, reg & 0xff, value);
}


static status_t
codec_read(hda_codec* codec, uint16 reg, uint8* _value)
{
	status_t status = codec_select_page(codec, reg);
	if (status != B_OK)
		return status;
	return i2c_read(codec, CS42L83_ADDRESS, reg & 0xff, _value);
}


static void
codec_update(hda_codec* codec, uint16 reg, uint8 mask, uint8 value)
{
	uint8 old = 0;
	if (codec_read(codec, reg, &old) == B_OK)
		codec_write(codec, reg, (old & ~mask) | (value & mask));
}


/*!	Takes the CS42L83 out of reset and sets it up as [D] does at boot, with
	its outputs off. Returns whether it answered.
*/
static bool
headphone_codec_init(hda_codec* codec)
{
	uint32 group = codec->audio_groups[0]->widget.node_id;

	// reset low, then high; it needs 3 ms after that [L]
	send_verb(codec, group, VID_SET_GPIO_EN, 0x0f);
	send_verb(codec, group, VID_SET_GPIO_DIR, CS42L83_GPIO_RESET);
	send_verb(codec, group, VID_SET_GPIO_DATA, 0);
	snooze(1000);
	send_verb(codec, group, VID_SET_GPIO_DATA, CS42L83_GPIO_RESET);
	snooze(5000);
	sCodecPage = -1;

	uint8 device = 0, revision = 0;
	if (codec_read(codec, CS42L83_DEVID_AB, &device) != B_OK
		|| codec_read(codec, CS42L83_REVID, &revision) != B_OK) {
		ERROR("the headphone codec does not answer\n");
		return false;
	}
	TRACE("headphone codec: device 0x%02x, revision 0x%02x\n", device,
		revision);

	codec_write(codec, 0x1103, 0x22);	// PWR_CTL3
	codec_write(codec, 0x1129, 0x01);	// HS_CLAMP_DISABLE
	codec_write(codec, 0x1320, 0x0f);	// TSRS_PLUG_INT_MASK: all masked
	codec_write(codec, 0x131b, 0x03);	// CODEC_INT_MASK
	codec_write(codec, 0x2302, 0x3f);	// MIXER_ADC_VOL: muted
	codec_write(codec, 0x1f01, 0x00);	// DAC_CTL1
	codec_write(codec, 0x1121, 0xa6);	// HS_SWITCH_CTL
	for (uint16 reg = 0x1316; reg <= 0x131a; reg++)
		codec_write(codec, reg, 0x00);	// interrupt masks
	codec_write(codec, 0x131c, 0x1a);	// SRCPL_INT_MASK
	codec_write(codec, 0x131e, 0x00);	// VPMON_INT_MASK
	codec_write(codec, 0x131f, 0x00);	// PLL_LOCK_INT_MASK

	// reading the status registers clears them
	static const uint16 kStatus[] = {
		0x1301, 0x1302, 0x1303, 0x1304, 0x1305, 0x130b, 0x130d, 0x130e,
		0x130f, 0x1308
	};
	for (size_t i = 0; i < B_COUNT_OF(kStatus); i++) {
		uint8 value;
		codec_read(codec, kStatus[i], &value);
	}

	codec_write(codec, CS42L83_PWR_CTL1, 0xfe);
		// the codec on, all its parts powered down
	codec_write(codec, 0x1b75, 0x9f);	// MIC_DET_CTL1
	codec_write(codec, CS42L83_TIPSENSE_CTL, 0xc0);
		// tip sense on, not inverted
	return true;
}


static bool
headphones_present(hda_codec* codec)
{
	uint8 status = 0;
	if (!sHeadphoneCodec
		|| codec_read(codec, CS42L83_DET_STATUS1, &status) != B_OK) {
		return false;
	}
	return (status & CS42L83_DET_TIP_SENSE) != 0;
}


//!	The CS8409's second serial port, to the CS42L83 [D].
static void
headphone_port_enable(hda_codec* codec)
{
	write_coef(codec, 0x15, 0xaaaa);
	write_coef(codec, 0x14, 0x0100);
	write_coef(codec, 0x29, 0x0800);
	write_coef(codec, 0x2a, 0x0820);

	uint16 config = read_coef(codec, CS8409_DEV_CFG1);
	write_coef(codec, CS8409_DEV_CFG1, 0xb000);
	write_coef(codec, 0x07, 0x10ff);
	write_coef(codec, CS8409_DEV_CFG1, 0xb000);
	write_coef(codec, CS8409_DEV_CFG1, config | 0x2000);

	write_coef(codec, 0x06, 0x8000);
	write_coef(codec, 0x08, 0x0040);
	update_coef(codec, 0x82, 0xa800, 0xa800);
	write_coef(codec, CS8409_DEV_CFG3, 0x0280);

	// sample rate fields, for 44.1 kHz
	update_coef(codec, 0x01, 0x0380, 0x0200);
	update_coef(codec, 0x08, 0x0007, 0x0002);
	update_coef(codec, 0x07, 0x01ff, 0x00ff);

	update_coef(codec, 0x01, 0x0040, 0x0040);
	write_coef(codec, 0x6c, 0x001f);
	update_coef(codec, 0x71, 0x800f, 0x800f);
	send_verb(codec, CS8409_NODE_VENDOR, CS8409_VID_TDM_COMMIT, 0xb6);
	send_verb(codec, CS8409_PIN_HEADPHONES, VID_SET_PIN_WIDGET_CONTROL,
		PIN_ENABLE_HEAD_PHONE | PIN_ENABLE_OUTPUT);
}


static void
headphone_port_disable(hda_codec* codec)
{
	write_coef(codec, 0x29, 0x8800);
	write_coef(codec, 0x2a, 0x8820);
	update_coef(codec, 0x82, 0xa800, 0);
	update_coef(codec, 0x01, 0x0040, 0);
	write_coef(codec, 0x71, 0);
	send_verb(codec, CS8409_NODE_VENDOR, CS8409_VID_TDM_COMMIT, 0);
	send_verb(codec, CS8409_PIN_HEADPHONES, VID_SET_PIN_WIDGET_CONTROL, 0);
}


//!	The CS42L83 playing its serial port to the headphones, at 44.1 kHz [D].
static void
headphone_codec_on(hda_codec* codec)
{
	// internal clocks and PLL, fixed for 44.1 kHz
	codec_write(codec, 0x1007, 0x10);	// FSYNC_P_LOWER area, as traced
	codec_write(codec, 0x1009, 0x02);
	codec_write(codec, 0x120c, 0x02);
	codec_write(codec, 0x1502, 0xff);	// PLL_DIV_FRAC0
	codec_write(codec, 0x1503, 0x9c);	// PLL_DIV_FRAC1
	codec_write(codec, 0x1504, 0x70);	// PLL_DIV_FRAC2
	codec_write(codec, 0x1505, 0x2e);	// PLL_DIV_INT
	codec_write(codec, 0x151b, 0x03);	// PLL_CTL4
	codec_write(codec, 0x150a, 0x5d);	// PLL_CAL_RATIO
	codec_write(codec, 0x1508, 0x10);	// PLL_CTL3
	codec_write(codec, 0x1501, 0x01);	// PLL_CTL1: start
	codec_write(codec, 0x1205, 0xff);
	codec_write(codec, 0x1206, 0x00);
	codec_write(codec, 0x1201, 0x00);
	codec_write(codec, 0x1208, 0x00);

	codec_write(codec, 0x1207, 0x20);
	codec_write(codec, 0x1501, 0x01);
	codec_write(codec, 0x1107, 0x01);

	// serial port, 44.1 kHz
	codec_write(codec, 0x2505, 0x05);
	codec_write(codec, 0x2502, 0x05);
	codec_write(codec, 0x2503, 0x8a);	// SP_RX_FS
	codec_write(codec, 0x2506, 0xca);	// SP_TX_FS
	codec_write(codec, 0x1208, 0x02);
	codec_write(codec, 0x2601, 0x4a);	// SRC_SDIN_FS
	codec_write(codec, 0x120a, 0x00);
	codec_update(codec, 0x1209, 0x01, 0x01);	// FS_RATE_EN

	// output path, muted for now
	codec_write(codec, 0x1f06, 0x02);
	codec_write(codec, CS42L83_HP_CTL, 0x0d);
	codec_write(codec, 0x2a04, 0x00);
	codec_write(codec, 0x2a07, 0x20);
	codec_write(codec, CS42L83_MIXER_CHA_VOL, sHeadphoneAttenuation);
	codec_write(codec, CS42L83_MIXER_CHB_VOL, sHeadphoneAttenuation);
	codec_write(codec, 0x2a02, 0x02);	// 24 bit samples
	codec_write(codec, 0x2a05, 0x02);
	codec_write(codec, 0x100b, 0x00);
	codec_write(codec, 0x2a01, 0x0c);	// serial port channels 1 and 2 on

	// power up, waiting for the DAC's sample rate converter to lock
	codec_update(codec, CS42L83_PWR_CTL1, 0x61, 0);
	bool locked = false;
	for (int i = 0; i < 20 && !locked; i++) {
		uint8 status = 0;
		if (codec_read(codec, CS42L83_SRCPL_INT_STATUS, &status) == B_OK)
			locked = (status & CS42L83_SRCPL_DAC_LOCKED) != 0;
		if (!locked)
			snooze(2000);
	}
	if (!locked)
		ERROR("headphone codec: DAC not locked, going on\n");
	codec_update(codec, CS42L83_PWR_CTL1, 0x08, 0);

	codec_write(codec, 0x107e, 0x99);
	codec_write(codec, 0x1b08, 0x30);
	codec_write(codec, 0x2102, 0x00);
	codec_write(codec, 0x107e, 0x00);
	codec_update(codec, CS42L83_HP_CTL, CS42L83_HP_MUTE, 0);
}


static void
headphone_codec_off(hda_codec* codec)
{
	codec_update(codec, CS42L83_HP_CTL, CS42L83_HP_MUTE, CS42L83_HP_MUTE);
	codec_write(codec, 0x107e, 0x99);
	codec_write(codec, 0x1b08, 0x10);
	codec_write(codec, 0x2102, 0x20);
	codec_write(codec, 0x107e, 0x00);
	codec_update(codec, CS42L83_PWR_CTL1, 0x08, 0x08);
	codec_update(codec, CS42L83_PWR_CTL1, 0x60, 0x60);
	codec_write(codec, 0x2a01, 0x00);
	codec_write(codec, 0x1107, 0x00);
	codec_write(codec, 0x1501, 0x00);	// PLL off
}


//	#pragma mark - outputs


static void
speakers_on(hda_codec* codec, hda_stream* stream)
{
	send_verb(codec, CS8409_NODE_SPEAKERS_A, VID_SET_CONVERTER_STREAM_CHANNEL,
		stream->id << 4);
	send_verb(codec, CS8409_NODE_SPEAKERS_B, VID_SET_CONVERTER_STREAM_CHANNEL,
		(stream->id << 4) | CS8409_SPEAKERS_B_CHANNEL);

	tdm_enable(codec);
	for (size_t i = 0; i < B_COUNT_OF(kAmplifiers); i++)
		amplifier_setup(codec, kAmplifiers[i].address, kAmplifiers[i].slot);
	sync_converters(codec, true);
	if (sTestTone)
		start_test_tone(stream);
}


static void
speakers_off(hda_codec* codec)
{
	sync_converters(codec, false);
	for (size_t i = 0; i < B_COUNT_OF(kAmplifiers); i++)
		amplifier_power_down(codec, kAmplifiers[i].address);
	tdm_disable(codec);
	send_verb(codec, CS8409_NODE_SPEAKERS_A, VID_SET_CONVERTER_STREAM_CHANNEL,
		0);
	send_verb(codec, CS8409_NODE_SPEAKERS_B, VID_SET_CONVERTER_STREAM_CHANNEL,
		0);
}


static void
headphones_on(hda_codec* codec, hda_stream* stream)
{
	// the same stream as the speakers would take, channels 0 and 1
	uint32 format = read_verb(codec, CS8409_NODE_SPEAKERS_A,
		VID_GET_CONVERTER_FORMAT) & 0xffff;
	send_verb(codec, CS8409_NODE_HEADPHONES, VID_SET_CONVERTER_FORMAT, format);
	send_verb(codec, CS8409_NODE_HEADPHONES, VID_SET_CONVERTER_STREAM_CHANNEL,
		stream->id << 4);

	headphone_port_enable(codec);
	headphone_codec_on(codec);
}


static void
headphones_off(hda_codec* codec)
{
	headphone_codec_off(codec);
	headphone_port_disable(codec);
	send_verb(codec, CS8409_NODE_HEADPHONES, VID_SET_CONVERTER_STREAM_CHANNEL,
		0);
}


//!	Switches the sound to the given output, or off. Called with sLock held.
static void
select_output(hda_codec* codec, hda_stream* stream, output_type output)
{
	if (output == sOutput)
		return;

	if (sOutput == OUTPUT_SPEAKERS)
		speakers_off(codec);
	else if (sOutput == OUTPUT_HEADPHONES)
		headphones_off(codec);

	send_verb(codec, CS8409_NODE_VENDOR, VID_SET_PROCESSING_STATE, 1);
	if (output == OUTPUT_SPEAKERS) {
		TRACE("sound to the speakers\n");
		speakers_on(codec, stream);
	} else if (output == OUTPUT_HEADPHONES) {
		TRACE("sound to the headphones\n");
		headphones_on(codec, stream);
	}
	sOutput = output;
}


//	#pragma mark - public


void
hda_cs8409_init(hda_codec* codec)
{
	if (!hda_cs8409_is_apple(codec) || codec->num_audio_groups == 0)
		return;

	MutexLocker locker(sLock);
	TRACE("MacBookPro14,1 speakers: setting up the I2C bus\n");

	void* settings = load_driver_settings("hda.settings");
	if (settings != NULL) {
		const char* value = get_driver_parameter(settings, "apple_amp_volume",
			NULL, NULL);
		if (value != NULL)
			sAmplifierVolume = strtoul(value, NULL, 0) & 0xff;
		sTestTone = get_driver_boolean_parameter(settings, "apple_test_tone",
			false, true);
		sDebug = get_driver_boolean_parameter(settings, "apple_debug", false,
			true);
		value = get_driver_parameter(settings, "apple_headphone_volume", NULL,
			NULL);
		if (value != NULL)
			sHeadphoneAttenuation = min_c(strtoul(value, NULL, 0), 63);
		unload_driver_settings(settings);
	}
	TRACE("amplifier volume 0x%02x, headphones -%u dB%s%s\n",
		sAmplifierVolume, sHeadphoneAttenuation,
		sTestTone ? ", test tone on" : "", sDebug ? ", debug logging" : "");

	// audio function group on, vendor processing on
	send_verb(codec, codec->audio_groups[0]->widget.node_id,
		VID_SET_POWER_STATE, 0);
	send_verb(codec, CS8409_NODE_VENDOR, VID_SET_PROCESSING_STATE, 1);
	vendor_init(codec);

	// See which amplifiers answer, then keep them off until playback.
	int answered = 0;
	for (size_t i = 0; i < B_COUNT_OF(kAmplifiers); i++) {
		uint8 address = kAmplifiers[i].address;
		uint8 power = 0;
		uint8 dac = 0;
		if (i2c_read(codec, address, AMP_POWER, &power) == B_OK
			&& i2c_read(codec, address, AMP_DAC_CONTROL, &dac) == B_OK) {
			answered++;
			TRACE("amplifier 0x%02x: power 0x%02x, DAC control 0x%02x\n",
				address, power, dac);
		}
		amplifier_power_down(codec, address);
	}
	TRACE("%d of %d amplifiers answered\n", answered,
		(int)B_COUNT_OF(kAmplifiers));

	sHeadphoneCodec = headphone_codec_init(codec);
	if (sHeadphoneCodec) {
		TRACE("headphones %s\n",
			headphones_present(codec) ? "plugged in" : "not plugged in");
	}
}


bool
hda_cs8409_use_playback_widget(hda_codec* codec, uint32 nodeID)
{
	if (!hda_cs8409_is_apple(codec))
		return true;

	return nodeID == CS8409_NODE_SPEAKERS_A || nodeID == CS8409_NODE_SPEAKERS_B
		|| nodeID == CS8409_PIN_SPEAKERS_A || nodeID == CS8409_PIN_SPEAKERS_B;
}


uint32
hda_cs8409_playback_channels(hda_codec* codec)
{
	// The amplifiers expect four TDM channels; a two channel stream comes
	// out as full-scale noise [D]. The user side fills channels 0 and 1; see
	// CS8409_SPEAKERS_B_CHANNEL for 2 and 3.
	return hda_cs8409_is_apple(codec) ? 4 : 0;
}


void
hda_cs8409_playback_start(hda_audio_group* audioGroup)
{
	hda_codec* codec = audioGroup->codec;
	if (!hda_cs8409_is_apple(codec))
		return;

	MutexLocker locker(sLock);
	TRACE("playback start\n");

	// Setting the stream up again puts the converters back on their
	// defaults without a stop, so set the output up from scratch.
	select_output(codec, audioGroup->playback_stream, OUTPUT_NONE);

	sLastJackReading = headphones_present(codec);
	sJackExchanges = 0;
	select_output(codec, audioGroup->playback_stream,
		sLastJackReading ? OUTPUT_HEADPHONES : OUTPUT_SPEAKERS);
	sExchanges = 0;
}


/*!	Logs the loudest sample in the playback buffers' first two channels, to
	see whether any audio reaches the driver at all. The buffers are mapped
	into the media server too, so they are only read through user_memcpy().
*/
static void
log_level(hda_stream* stream)
{
	if (stream->sample_size != 4 || stream->num_channels < 2)
		return;

	int32 peak = 0;
	int32 chunk[256];
	uint32 framesPerChunk = B_COUNT_OF(chunk) / stream->num_channels;
	for (uint32 i = 0; i < stream->num_buffers; i++) {
		for (uint32 frame = 0; frame < stream->buffer_length;
				frame += framesPerChunk) {
			uint32 frames = min_c(framesPerChunk,
				stream->buffer_length - frame);
			if (user_memcpy(chunk, (int32*)stream->buffers[i]
					+ frame * stream->num_channels,
					frames * stream->num_channels * sizeof(int32)) != B_OK) {
				return;
			}
			for (uint32 j = 0; j < frames; j++) {
				for (uint32 channel = 0; channel < 2; channel++) {
					int32 sample = chunk[j * stream->num_channels + channel];
					if (sample < 0)
						sample = sample == INT32_MIN ? INT32_MAX : -sample;
					if (sample > peak)
						peak = sample;
				}
			}
		}
	}
	TRACE("audio level: peak 0x%08" B_PRIx32 " in %" B_PRIu64 " frames\n",
		peak, stream->frames_count);
}


void
hda_cs8409_playback_running(hda_audio_group* audioGroup)
{
	hda_codec* codec = audioGroup->codec;
	if (!hda_cs8409_is_apple(codec))
		return;

	if (sOutput == OUTPUT_SPEAKERS && !sTestTone)
		copy_to_second_pair(audioGroup->playback_stream);

	// Follow the jack, once two checks in a row agree.
	if (sHeadphoneCodec && ++sJackExchanges >= kJackCheckEveryExchanges) {
		sJackExchanges = 0;
		MutexLocker locker(sLock);
		bool present = headphones_present(codec);
		output_type wanted = present ? OUTPUT_HEADPHONES : OUTPUT_SPEAKERS;
		if (present == sLastJackReading && wanted != sOutput) {
			TRACE("headphones %s\n", present ? "plugged in" : "unplugged");
			select_output(codec, audioGroup->playback_stream, wanted);
		}
		sLastJackReading = present;
	}

	if (!sDebug)
		return;

	sExchanges++;
	if (sExchanges % kLevelEveryExchanges == 0 && sLevelLogs < kLevelLogs) {
		sLevelLogs++;
		log_level(audioGroup->playback_stream);
	}

	for (size_t i = 0; i < B_COUNT_OF(kDumpAtExchange); i++) {
		if (sExchanges == kDumpAtExchange[i]) {
			MutexLocker locker(sLock);
			dump_state(codec);
		}
	}
}


void
hda_cs8409_playback_stop(hda_audio_group* audioGroup)
{
	hda_codec* codec = audioGroup->codec;
	if (!hda_cs8409_is_apple(codec))
		return;

	MutexLocker locker(sLock);
	TRACE("playback stop\n");
	select_output(codec, audioGroup->playback_stream, OUTPUT_NONE);
}
