# RenkuOS on the MacBookPro14,1: handoff

Last updated 2026-11-26. Read this first when continuing on another
computer. This branch (`notes`) holds only notes; the code is on the
`wifi` branch of this repository (everything on `apfs`, plus the newer
camera work and the built-in Wi-Fi).

## Where things are

- Code: github.com/Drake3d/renkuos-macbook.
  - `wifi` (work here): `apfs` plus 3 camera commits (settings kept across
    restarts, idle power-down, picture sizes/mirror/CodyCam settings) and
    4 Wi-Fi commits. Tip: `419c2da7d4` "brcmfmac: real link rate and
    signal, receive drop warning". Pushed 2026-11-26.
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
  attributes, compressed files (LZVN, LZFSE, zlib), programs run from
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
5. Microphone is quiet; optional higher default gain.
6. After the merge, check the internal microphone and the input selector
   (upstream reworked hda mux controls) and display moves on Kaby Lake.
7. APFS: names typed in a different Unicode normalization than macOS
   stored (needs NFD and case-folding tables in the kernel); compressed
   files of type 4 (zlib in a classic resource fork, Mac OS X 10.6-10.10,
   refused today); snapshots; FileVault volumes; write support.

## Notes in this branch

- camera-facetimehd-design.md: camera hardware, sources, phase results.
- sound-cs8409-design.md: CS8409 audio design and provenance.
- wifi-bcm4350-plan.md: Wi-Fi plan and the result of each phase.
- codycam-settings-plan.md: shelved plan for CodyCam's settings window.
- apfs-readonly-design.md: APFS driver design, results, and the places
  where real volumes differ from Apple's reference.

## APFS test images

On "NEW FAT VOL" in `apfs-test/` (plain, multi, stress, attrs, comp),
with a README of MacBook test steps. `make_test_images.sh` in
`src/tests/add-ons/kernel/file_systems/apfs/` rebuilds them on a Mac
(comp.dmg by hand, as described there). Attach test images on the Mac
only with `hdiutil attach -readonly`: a normal mount changes them.
