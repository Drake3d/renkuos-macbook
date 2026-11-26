# MacBookPro14,1 sound: CS8409 design and provenance

Status: design only, nothing implemented. Approach agreed: learn from the Linux
drivers, write new MIT code, record where every fact came from (option 1).

## 1. Hardware, from the machine's syslog (verified)

From `/Volumes/NEW FAT VOL/syslog`, latest boot, Haiku `hda` driver:

- Controller: Intel HDA at PCI 0:31.3, HDA v1.0, 9 output / 7 input streams.
- Codec 0: **Cirrus Logic CS8409**, `1013:8409`, subsystem **`106b:3300`**,
  revision 1.0.1.0. The audio function group reports 8 GPIOs, unsolicited
  responses supported.
- Codec 1: the Intel HDMI audio of the graphics, 2 widgets. Its "No such device"
  failure does not matter for the speakers.
- CS8409 widgets (decimal node IDs):

| Node | Kind | Notes |
|---|---|---|
| 2–17 | audio outputs | each feeds one output pin, 36–51 |
| 18–35 | audio inputs | 34, 35 have input amps, 63 steps of 1 dB |
| 36, 37 | pins, fixed **Speaker**, assoc 1 | fed by 2 and 3 |
| 44 | pin, **headphone jack** (green, assoc 2) | fed by 10; sensed unplugged at boot |
| 60 | pin, **mic jack** (pink, assoc 3) | feeds input 26 |
| 68 | pin, fixed **internal digital mic** | feeds input 34 |
| 70 | beep generator | |
| 71 (0x47) | vendor-defined widget | the I2C/coefficient interface |

- Haiku already builds streams on the CS8409:
  - playback on outputs 2–9,
  - recording from inputs 26 and 34.

  Audio reaches the CS8409, and it's silent only because the amplifiers and the
  headphone chip behind it are never set up.

## 2. How the hardware works (learned facts)

Sources:
- **[M]** mainline Linux `patch_cs8409.c/.h`, `patch_cs8409-tables.c` (v6.6).
  Written by Cirrus Logic. GPL-2.0-or-later / GPL-2.0-only.
- **[D]** davidjo/snd_hda_macbookpro. GPL-2.0. Apple values traced from macOS
  AppleHDA.

The confidence of each fact is noted below. Values marked *verify* were seen only in a
summary of a truncated page and must be re-read before use.

### 2.1 Machine
- `106b:3300` is the MacBookPro14,1 per a comment in [D] `patch_cirrus_new84.h`.
  [D]'s quirk table *may* call it 13,1 (*verify*). Either way, the same code path
  is used.
- Speakers: **four SSM3515-type amplifiers** at I2C 0x28, 0x2a, 0x2c, 0x2e, on
  channels 0, 2, 1, 3 [D `patch_cirrus_real84.h`]. They form two stereo pairs:
  tweeters on node 2 → pin 36, woofers on node 3 → pin 37 [D NOTES.md, README].
- Headphones and headset mic: **CS42L83** at 0x90 (0x48 shifted) [D `cirrus_apple.h`].
  Reset is on CS8409 GPIO1 (0x02) and its interrupt on GPIO0 (0x01).

### 2.2 CS8409 as an I2C master [M], matching [D]
- All access goes through the coefficient verbs on node 0x47:
  - `SET_COEF_INDEX` (0x500)
  - `SET_PROC_COEF` (0x400)
  - `GET_PROC_COEF` (0xC00)
  - `SET_PROC_STATE` = 1 first.
- Coefficients:
  - 0x00 DEV_CFG1: bit 0x8 is the I2C clock enable.
  - 0x02 DEV_CFG3: set to 0x0080 for I2C mode.
  - 0x5b I2C_CTRL: set to 0x0200 for 100 kHz.
  - 0x59 I2C address.
  - 0x5c status: done when `& 0x18`.
  - 0x5d paged write.
  - 0x5e paged read.
- **Write:**
  1. Clock on.
  2. Address to 0x59.
  3. If the page changed, write the page (`reg >> 8`) to 0x5d and poll.
  4. Write `(reg << 8 & 0xff00) | data` to 0x5d and poll.
- **Read:**
  1. Write `(reg << 8) & 0xffff` to 0x5e and poll.
  2. Read 0x5e; the data is in the low byte.
- **Polling:** about every 2 ms, giving up after about 20 ms.

### 2.3 Set-up for 106b:3300 [D], counts and structure
- **CS8409 TDM set-up:** about 11 coefficient writes. They set the ASP1 transmit slots,
  clocks, the sample-rate fields in coefficients 0x01, 0x04 and 0x05, and pad
  configuration, then send vendor verb 0x7f0 = 0x00b6. Pins 36 and 37 get
  pin control OUT_EN.
- **Each SSM3515:** 7 register writes and 2 reads (channel, mode, volume, power-up).
  Volume 0x48 is about −24 dB, a deliberate reduction in [D].
- **Converter sync:** coefficient 0x17 is written around nodes 2 and 3.
- **Playback format:** 44.1 kHz only, 24- or 32-bit, **4 channels**: stereo
  duplicated to both pairs [D]. A 2-channel stream on this machine gives
  **full-scale noise** (davidjo issue #217).
- **Boot order (*verify*):**
  1. Function group to D0.
  2. Enable I2C.
  3. GPIOs.
  4. CS42L83 reset and init.
  5. Amps.
  6. Jack sense.
  7. TDM, amps.
  8. Converter sync.

### 2.4 Headphone jack [D]
- The CS42L83 interrupt arrives as an unsolicited response; its low byte mirrors the
  GPIO state.
- CS42L83 registers:
  - 0x1b77 headphone sense,
  - 0x1124 headset type,
  - 0x1b78 mic detect,
  - 0x1b79 plug interrupt mask,
  - 0x131b automatic headset detection.
- With headphones in, the speakers are switched off.

## 3. Design for Haiku

The code goes **inside `src/add-ons/kernel/drivers/audio/hda`**, because a separate
driver would fight `hda` for the controller. It builds into the nightly image. It can't
be a separately installable package, because `hda` lives in the `haiku` package.

New files (MIT, SPDX):
- `hda_cs8409.cpp/.h`: the I2C-over-coefficient helper, GPIO helpers, the
  CS8409 TDM set-up, the SSM3515 and CS42L83 set-up, and jack handling.

Hooks into existing `hda` code (small changes):
1. **Codec probe** (`hda_codec_new`): when the codec is `1013:8409` with
   subsystem `106b:3300`, run the CS8409 boot set-up and mark the codec.
2. **Widget tree:**
   - Only offer node 2 → pin 36, node 3 → pin 37 and node 10 → pin 44 for playback.
   - Offer pins 68 and 60 for recording.
   - Hide the unused outputs 4–9 and 11–17. They're unconnected ASP slots, and
     today they're folded into the playback stream.
3. **Stream format:** force 44.1 kHz, 24-in-32-bit, 4 channels on the speaker
   stream. If Haiku's mixer stays stereo, the driver duplicates the pair into
   channels 3–4.
4. **Stream start/stop:** amplifiers on at start, off at stop, to avoid pops and
   save power.
5. **Unsolicited response:** jack plug and unplug, switching between speakers and
   headphones.

## 4. Phases

| Phase | Goal | Done when |
|---|---|---|
| 1 | Speakers, fixed safe volume, 44.1 kHz | MediaPlayer plays through both speaker pairs |
| 2 | Volume control | System volume works; amps stay at a safe level |
| 3 | Headphone jack | Plug/unplug switches outputs; headphones play |
| 4 | Internal mic, then headset mic | Recording works (also unfinished in [D]) |
| 5 | Power: suspend, idle | Amps off when idle; survives a reboot without pops |

**Before phase 1, add a debug log** of every I2C transaction and coefficient write,
so a failed boot can be read from the syslog.

## 5. Safety

- A wrong format gives **full-scale noise** (issue #217). For the first tests:
  - set system volume low,
  - keep headphones off your ears,
  - start the amplifiers at the reduced 0x48 level.
- Amplifier register meanings will be cross-checked against the public
  **SSM3515 datasheet** (Analog Devices) before use, not taken from [D] alone.

## 6. Provenance rules

- New code is written from the facts above, not from [M] or [D] code. No code
  structure, names or comments are carried over.
- Each register write in the driver gets a comment naming its source:
  - a datasheet section,
  - [M], for the CS8409 I2C interface written by Cirrus,
  - [D], for Apple trace values.
- Values from [D] were traced from macOS by the davidjo project and published in
  a GPL repository. For personal use this is fine. **Before sending anything to
  Haiku, review these tables:** reproducing them closely may count as copying
  GPL work. Where possible, re-derive amplifier and CS42L83 values from the
  public datasheets (SSM3515; the CS42L42 datasheet for the CS42L83's shared
  register map).
- Commit messages carry `Assisted-by:` and describe the sources as above.

## 7. Open items to verify

- The full 106b:3300 boot sequence in [D] `patch_cirrus_real84.h` (was truncated).
- Whether 0x28–0x2e are 8-bit (shifted) addresses, as 0x90 is.
- The GPIO masks for 106b:3300 (0x0f?) and the meaning of the 0xd/0xf arguments.
- How Haiku's `hda_multi_audio` channel layout maps to the 4-channel TDM
  stream.

## 8. Microphone gain (2026-11-26, `e707a92fb1`)

- Pin 0x44 boost: 0, 10 or 20 dB; the driver uses 20 (`apple_mic_boost`).
- Converter 0x22 input amplifier: -51 to +12 dB in 1 dB steps (offset 51,
  63 steps), read from the chip. The generic code leaves it on 0 dB; the
  driver now starts it at +9 dB (`apple_mic_gain <dB>` in hda.settings).
- Speech recorded at normal distance: quiet at +3 dB, good but a little
  loud at +12 dB, hence +9 dB.
- The media server saves the mixer's recording gain
  (`Media/multi_audio_settings`) and sets it again at start-up, after the
  driver, so a saved slider position wins over the driver's default.
