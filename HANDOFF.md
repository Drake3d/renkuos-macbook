# RenkuOS on the MacBookPro14,1: handoff

Last updated 2026-11-26. Read this first when continuing on another
computer. This branch (`notes`) holds only notes; the code is on the
`wifi` branch of this repository (everything on `apfs`, plus the newer
camera work and the built-in Wi-Fi).

## Where things are

- Code: github.com/Drake3d/renkuos-macbook.
  - `wifi` (work here): `apfs` plus 3 camera commits (settings kept across
    restarts, idle power-down, picture sizes/mirror/CodyCam settings) and
    4 Wi-Fi commits, then the microphone gain commit (`e707a92fb1`), APFS
    type 4 compression (`9b7dae4981`), APFS Unicode names (`bd51ec5568`)
    the Apple SMC sensor driver (`1df69d3307`) and two FAT driver fixes.
    Tip: `c680fd5d2a` "fat: fix two panics, mkdir of an existing folder
    and truncating read-only". Pushed 2026-11-26.
  - `apfs`: `apple-spi-input` plus the APFS driver, milestones 1-5
    (`personal/apfs` tip `8d6d75b8a4`). The 3 camera commits sit on the
    local `apfs` too but were pushed only as part of `wifi`.
  - `apple-spi-input`: everything before APFS; on GitHub `apfs` was merged
    into it (`9ad3016a27`, pull request #1 on the personal repository).
    Before that its tip was `24f9dc8e84` "Merge RenkuOS main into
    apple-spi-input" (RenkuOS `main` at `7b0177e735`).
- Remotes in a working clone:
  - `origin` = https://github.com/RenkuOS/Source.git (read only for us)
  - `personal` = https://github.com/Drake3d/renkuos-macbook.git

### Setting up a new computer

```
git clone https://github.com/RenkuOS/Source.git
cd Source
git remote add personal https://github.com/Drake3d/renkuos-macbook.git
git fetch personal
git switch -c wifi --track personal/wifi
```

Then create `build/jam/UserBuildConfig` (git-ignored). It puts every
package of this work into each image (so one image tests the MacBook and
other machines), and adds the camera files only if it finds them:

```
# Personal build settings (git-ignored).

# Optional packages built from this tree; add new ones here.
local personalPackages =
	AppleSMC		# Apple SMC sensors: temperatures and fan speeds
	AppleSPIInput	# MacBook SPI keyboard and trackpad, media and volume keys
	FaceTimeHD		# FaceTime HD camera: driver, media add-on, Camera prefs
	APFS			# read-only Apple File System driver
	BroadcomWifi	# BCM4350 PCIe Wi-Fi
	;
AddOptionalHaikuImagePackages $(personalPackages) ;

# Apple's camera files, for personal test images only; never committed.
# Set RENKU_FTHD_FILES to the folder holding fthd-extract/firmware.bin and
# setfile-extract/1571_01XX.dat if it is not /Volumes/Source/working.
local fthdWorking = $(RENKU_FTHD_FILES:E=/Volumes/Source/working) ;
local fthdFirmware = [ Glob $(fthdWorking)/fthd-extract : firmware.bin ] ;
local fthdSetFile = [ Glob $(fthdWorking)/setfile-extract : 1571_01XX.dat ] ;
if $(fthdFirmware) && $(fthdSetFile) {
	AddFilesToHaikuImage system non-packaged data firmware facetimehd
		: $(fthdFirmware) $(fthdSetFile) ;
} else {
	Echo "UserBuildConfig: FaceTime HD firmware not found in"
		"$(fthdWorking); the image will not include it." ;
}

# Broadcom's BCM4350 Wi-Fi firmware from a linux-firmware checkout, for
# personal test images only; never committed. Set RENKU_LINUX_FIRMWARE if
# the checkout is not /Volumes/Source/linux-firmware.
local linuxFirmware = $(RENKU_LINUX_FIRMWARE:E=/Volumes/Source/linux-firmware) ;
local brcmFirmware = [ Glob $(linuxFirmware)/brcm
	: brcmfmac4350c2-pcie.bin brcmfmac4350-pcie.bin ] ;
if $(brcmFirmware) {
	AddFilesToHaikuImage system non-packaged data firmware brcm
		: $(brcmFirmware) ;
} else {
	Echo "UserBuildConfig: BCM4350 Wi-Fi firmware not found in"
		"$(linuxFirmware)/brcm; the image will not include it." ;
}
```

Already part of every image without a setting: the CS8409 sound (hda),
Kaby Lake brightness (intel_extreme) and the EFI start-up fix. With the
camera files found, a freshly installed image has a working camera
(tested 2026-11-25); without them, copy them by hand (see "Camera").

Configure and build as usual for Haiku (x86_64), in `generated.x86_64`:
`jam -q -j<cores> @nightly-anyboot`, or single targets such as
`facetimehd.hpkg`, `apple_spi_input.hpkg`, `apfs.hpkg`,
`broadcom_wifi.hpkg`, `apfs`, `hda`.
APFS test shell (runs the driver on macOS against an image file):
`jam -q '<build>apfs_shell'`, then
`objects/darwin/arm64/release/tests/add-ons/kernel/file_systems/apfs/apfs_shell/apfs_shell <image>`
(`APFS_VOLUME=<index|name>` picks a volume).

Testing a driver on the MacBook: the kernel looks for drivers in
`/boot/system/add-ons` (from packages) before
`/boot/system/non-packaged/add-ons`, so once a driver's package is
installed (images include `apfs.hpkg`), a test copy in `non-packaged` is
silently ignored and the old driver keeps running. Install the new
`.hpkg` in `/boot/system/packages` instead, as for Wi-Fi and the camera.
A driver inside the `haiku` package itself (such as `hda`) has no package
of its own: hide the packaged one with a `/boot/system/settings/packages`
file (`Package haiku { BlockedEntries { add-ons/kernel/drivers/bin/hda
add-ons/kernel/drivers/dev/audio/hmulti/hda } }`), put the test copy in
`non-packaged/add-ons/kernel/drivers/bin/` with the `dev/audio/hmulti/`
symlink to it, and reboot; remove all three to go back.

### Other architectures (built 2026-11-26)

Cross tools (GCC 13.3.0) are built in `generated.arm64` (`aarch64-unknown-haiku`)
and `generated.x86` (`i586-pc-haiku`), about 250 MB each, with
`../configure --cross-tools-source ../../buildtools --build-cross-tools <arch> -j8`
(one at a time: the gcc build tree needs a few GB while it runs).

- arm64: `jam -q -j8 brcmfmac` works. The RenkuOS arm64 package server
  (`renkuos.freemyip.com/repo/renkuosports/arm64`) keeps every package in
  one flat `packages/` folder, but jam fetches `<url>/<checksum>/packages/`,
  so downloads 404. Do what CI does: fetch the package jam names from
  `<url>/packages/<file>` into `generated.arm64/download/` and run jam again
  (the driver needed only `gcc_syslibs_devel-13.3.0_2023_08_10-7-arm64.hpkg`).
- Plain x86 cannot build: `build/jam/repositories/HaikuPorts/x86` dates from
  2018 and upstream has no snapshot for it (needs `gcc_syslibs_devel-5.4.0`,
  404). RenkuOS's 32-bit image is the `x86_gcc2h` hybrid (GCC 2.95 plus GCC 13
  for x86), and GCC 2.95 cannot be built on an Apple Silicon Mac: its build
  needs a 32-bit host compiler (`-m32`). CI builds it on Linux; a Linux machine
  or the CI cross-compiler container would be needed here. For a compile
  check, take jam's commands (`jam -n -a -dx '<src!...>file.o'` in
  `generated.x86`) and point the `gcc_syslibs_devel` header paths at
  `cross-tools-x86/i586-pc-haiku/include/c++/13.3.0` and
  `cross-tools-x86/lib/gcc/i586-pc-haiku/13.3.0/include{,-fixed}`.

Disk space: `/Volumes/Source` is an 18 GB image and fills up; on
2026-11-25 builds failed with 112 MB free. Keep large test images
elsewhere (the APFS ones are on "NEW FAT VOL" in `apfs-test/`).

Build host gotcha (macOS): host tools under `generated.x86_64/objects/darwin`
can stay linked to a library that is gone (in November they pointed at
MacPorts' `/opt/local/lib/libz.1.dylib` after MacPorts was removed). If a
tool aborts with "dyld: Library not loaded", find them with
`otool -L <tool> | grep /opt/local`, delete those tools, and run jam again.

### Updating from RenkuOS main

Done once on 2026-11-25 by merging (not rebasing, so pushed history stays):

```
git fetch origin
git branch -f main origin/main
git switch wifi
git merge --no-ff origin/main
```

Watch for: new shared HID sources in `hid_shared/` that every HID driver
Jamfile lists (the `apple_spi` Jamfile must list them too; last time
`MouseReportDecoder.cpp`), and `drivers/input/Jamfile` conflicts.
Build, test the nightly on the MacBook, then commit the merge.

## Rules we work by

- Keep this work personal until stable: push only to `personal`, never to
  RenkuOS/Source and no PRs there.
- Commit only after the user has tested on the MacBook.
- `git commit -s`, with the trailers
  `Assisted-by: Claude Opus 5.5` and
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Never commit Apple or Broadcom firmware; the user supplies it.
- New code is MIT with SPDX lines. Linux drivers (GPL) are used for facts
  only, never code; each file names its sources. Exception: the new
  brcmfmac PCIe files follow OpenBSD's bwfm(4) closely and carry its ISC
  notices, like the rest of that driver.
- The user handles their own logins.
- Testing: the user boots the nightly ISO (or installs a new .hpkg into
  /boot/system/packages and reboots) and sends the syslog via the FAT
  USB drive ("NEW FAT VOL"). Read it with `grep -a` (binary bytes).

## Done (all on `apple-spi-input`, tested unless noted)

- apple_spi: internal keyboard and trackpad (SPI), media keys package.
- Volume, mute, brightness (F1/F2) and media keys (F7-F9) via the
  media_keys input_server filter.
- intel_extreme: Kaby Lake ULT GT3 treated as mobile (brightness works).
- hda (CS8409): speakers, headphone jack, internal microphones.
- FaceTime HD camera (5 commits): driver `facetimehd`, test tool
  `facetimehd_capture`, media add-on (CodyCam works, ~30 fps), Camera
  preferences app, package `facetimehd.hpkg`.
- Camera, 3 more commits (on `wifi`): picture settings kept across
  restarts (~/config/settings/kernel/drivers/facetimehd); camera powered
  down 15 s after use; any even picture size up to 1280x720 (tested
  64x48 to 1280x720 at 30 fps), Mirror and Picture size settings, and
  CodyCam's Video settings with the same controls as the Camera prefs.
- Merged RenkuOS `main` (711 commits) on 2026-11-25; nightly tested on
  the MacBook, including a fresh install with the camera files built in.
- APFS, read-only (branch `apfs`, 5 commits; see apfs-readonly-design.md):
  containers and volumes, folders, files, symlinks, hard links, extended
  attributes, compressed files (every decmpfs type: LZVN, LZFSE, zlib,
  type 4 since `9b7dae4981`), names in any Unicode form and, on
  case-insensitive volumes, any case (`bd51ec5568`), programs run from
  APFS, free space as macOS reports it; package `apfs.hpkg` (optional
  image package APFS). Tested on images and on two 4 TB macOS SSDs.
- Built-in Wi-Fi, BCM4350 (branch `wifi`, 4 commits; see
  wifi-bcm4350-plan.md): the brcmfmac driver gained a PCIe side (firmware
  start, msgbuf rings on MSI, data path with a flow ring per priority)
  under its existing Wi-Fi logic, and both buses now share the chip code
  (brcmf_chip.cpp). Scans, joins WPA2 networks, DHCP, ping with no loss;
  leaving, dropped links and moving to another access point all
  reconnect; 5 GHz at 400 Mbit/s; real link rate and signal in ifconfig.
  Package `broadcom_wifi.hpkg` (optional image package BroadcomWifi).
- Apple SMC sensors (`1df69d3307`): new driver
  `drivers/sensor/apple_smc/` for the ACPI device APP0001 (ports
  0x300/0x304 from _CRS). Read only; `cat /dev/sensor/apple_smc/0` lists
  each fan (current/min/max rpm) and every sp78 temperature key, common
  ones named; one set of readings in the syslog at boot. On the MacBook:
  798 keys, 33 temperatures, one fan; idle 42 C with the fan stopped, full
  load 82 C and 1380 rpm. Package `apple_smc.hpkg` (optional image
  package AppleSMC). Install it with pkgman (see below).
- Hardware survey (2026-11-26, survey/survey.sh on "NEW FAT VOL"): the
  battery, AC adapter, lid, power button (ACPI drivers), ambient light
  sensor (acpi_als) and keyboard backlight (apple_spi) already work.
  Still missing: Bluetooth (BCM2E7C on the Intel LPSS UART 8086:9d27,
  needs a UART driver, an H4/H5 transport and Broadcom's firmware patch);
  sleep (no system suspend). List of planned drivers:
  planned-drivers.txt on the drive.

## Standalone drivers and edits to RenkuOS code

Compared with RenkuOS main `7b0177e735` (the `wifi` merge base). Four
drivers are standalone: only new folders, plus the build registration (one
`SubInclude` line in the parent `Jamfile`, `HaikuPackages`,
`OptionalPackages`). They can go into any RenkuOS tree by copying their
folders and adding those lines.

| Driver | Package | New code |
|---|---|---|
| APFS (read-only) | `apfs.hpkg` | `file_systems/apfs/` (with LZFSE), test shell and image scripts in `src/tests/` |
| Apple SPI keyboard and trackpad | `apple_spi_input.hpkg` | `bus_managers/spi/`, `busses/spi/pch/`, `drivers/input/apple_spi/`, `input_server/filters/media_keys/`, `headers/private/spi/spi.h` |
| FaceTime HD camera | `facetimehd.hpkg` | `drivers/video/facetimehd/`, media add-on, Camera preferences, `facetimehd_capture` |
| Apple SMC sensors | `apple_smc.hpkg` | `drivers/sensor/apple_smc/` |

These changed existing RenkuOS code, so they must be merged into each
tree's version of those files:

| Change | Existing files edited |
|---|---|
| CS8409 sound | `hda_codec.cpp`, `hda_multi_audio.cpp`, `hda` `Jamfile` (new `hda_cs8409.*` holds most of it) |
| BCM4350 Wi-Fi | `brcmfmac` `brcmf.cpp`, `brcmfmac.h`, `driver.cpp`, `Jamfile` (PCIe side in new files) |
| Kaby Lake brightness | `intel_extreme` `mode.cpp` and the graphics `driver.cpp` |
| EFI start-up fix | `boot/platform/efi/arch/x86_64/arch_start.cpp` |
| FAT driver fixes | `file_systems/fat/kernel_interface.cpp` (`dosfs_mkdir`, `dosfs_open`) |

Every edited file, with line counts: changed-files.txt.

## Camera: files the user must supply

Both go in `/boot/system/non-packaged/data/firmware/facetimehd/` on the
MacBook. A fresh ISO wipes them unless the `AddFilesToHaikuImage` block in
`UserBuildConfig` (above) builds them into the image; otherwise copy them
again after reflashing. Never commit them.

- `firmware.bin` (sha256 240ef2e9...4330, 1,417,220 bytes): from
  AppleCameraInterface in macOSUpdCombo10.12.6.dmg; gzip at offset 81920,
  602,903 bytes. The pkg payload is pbzx; decode it and extract
  `*AppleCameraInterface*` with cpio.
- `1571_01XX.dat` (md5 083cf784ca19bf7d5698096eec636879, 18,652 bytes):
  from AppleCamera.sys in Boot Camp package 041-89042 (BootCampESD.pkg),
  WindowsSupport.dmg, AppleCamera64.exe (solid RAR, needs `unar`);
  `dd bs=1 skip=1937824 count=18652`.

## Wi-Fi: files the user must supply

`brcmfmac4350c2-pcie.bin` (608 KB; the MacBook's chip is revision 5) and,
for later chip revisions, `brcmfmac4350-pcie.bin`, from linux-firmware
(`brcm/`), go in `/boot/system/non-packaged/data/firmware/brcm/`. The
UserBuildConfig block above puts them into personal images. Never commit
them.

## Open items

1. Wi-Fi: test on a Raspberry Pi 4/400 (the SDIO side now uses
   brcmf_chip.cpp). On 2026-11-26 it built for arm64 with no warnings and
   every kernel symbol it uses resolves against the arm64 kernel.so; not
   run on a Pi yet. 32-bit x86: all sources compile with GCC 13
   (i586, kernel flags, no warnings), but it is not linked and not
   compiled with GCC 2.95 (see "Other architectures"). Tested
   on the MacBook: Network preferences, 5 GHz at 400 Mbit/s, a 140 MB
   download (at least 19 Mbit/s), uploads at ~27 Mbit/s, idle. Not tried:
   sleep (RenkuOS has no system suspend). For speed tests use
   cdn.kernel.org: Cloudflare's __down sends curl 1 byte and
   speedtest.tele2.net did not answer.
2. CodyCam's Video settings use the generic media theme window; making it
   look like the Camera prefs needs a CodyCam change, shelved by choice
   (codycam-settings-plan.md).
3. Camera: sleep and wake (needs system suspend, which RenkuOS lacks).
4. Speakers sometimes silent until a reboot (seen once): on recurrence,
   try Media preferences > Restart media services and save the syslog.
5. After the merge, check the input selector (upstream reworked hda mux
   controls) and display moves on Kaby Lake. The internal microphone
   records fine on the merged tree (2026-11-26).
6. The MacBook still runs the test hda from `hda-mic-test` (a copy in
   non-packaged plus `/boot/system/settings/packages` hiding the haiku
   package's hda). Run its `uninstall.sh` and reboot when an image with
   `e707a92fb1` is installed.
7. APFS: sealed volumes (every macOS 11+ System volume, and the cryptex
   images in Preboot) do not mount: their file-system tree nodes fail the
   checksum check ("bad checksum in block ..."), as sealed trees are
   protected by hashes instead; data volumes are not sealed. FileVault
   volumes; write support. Snapshots were skipped by choice (unlikely to
   be needed on RenkuOS); the started code (listing them at mount, a
   "snapshot=" mount option) is in working/apfs-snapshots-shelved.patch,
   untested: macOS will not create snapshots on disk images, even as root
   (fs_snapshot_create needs an Apple entitlement).
8. FAT driver (RenkuOS's own code): two panics fixed in `c680fd5d2a`
   (mkdir of an existing folder; O_TRUNC on a read-only volume or file),
   tested on the MacBook with the driver from fat-test/ on "NEW FAT VOL"
   loaded in place of the packaged one (BlockedEntries, like the hda
   test). Not run there: overwriting on a volume mounted read-only, the
   exact case of the second panic. The MacBook still runs that test
   driver: run fat-test/uninstall.sh and reboot once an image with
   `c680fd5d2a` is installed (it keeps the hda test entries, if any).
   Images before `c680fd5d2a` still have both panics: do not mkdir -p or
   overwrite files on FAT drives with them (dragging in Tracker is safe).
9. Installing a package by copying it into /boot/system/packages can
   fail: the package daemon may read it while it is still being copied
   ("failed to init package for file") and never activates it. Use
   `pkgman install <file>.hpkg` (macbook-drivers/install.sh does).

## Notes in this branch

- camera-facetimehd-design.md: camera hardware, sources, phase results.
- sound-cs8409-design.md: CS8409 audio design and provenance.
- wifi-bcm4350-plan.md: Wi-Fi plan and the result of each phase.
- changed-files.txt: every existing RenkuOS file we changed, with line
  counts, grouped by feature.
- codycam-settings-plan.md: shelved plan for CodyCam's settings window.
- apfs-readonly-design.md: APFS driver design, results, and the places
  where real volumes differ from Apple's reference.

## APFS test images

On "NEW FAT VOL" in `apfs-test/` (plain, multi, stress, attrs, comp),
with a README of MacBook test steps. `make_test_images.sh` in
`src/tests/add-ons/kernel/file_systems/apfs/` rebuilds them on a Mac
(comp.dmg by hand, as described there). Attach test images on the Mac
only with `hdiutil attach -readonly`: a normal mount changes them.
