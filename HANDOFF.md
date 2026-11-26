# RenkuOS on the MacBookPro14,1: handoff

Last updated 2026-11-25. Read this first when continuing on another
computer. This branch (`notes`) holds only notes; the code is on the
`apfs` branch of this repository (everything on `apple-spi-input`, plus the
APFS driver).

## Where things are

- Code: github.com/Drake3d/renkuos-macbook.
  - `apfs` (work here): `apple-spi-input` plus the APFS driver, milestones
    1-5. Local tip: `8d6d75b8a4` "apfs: milestone 5, package, free space and
    hardening". Push it before switching computers (`git push` on `apfs`);
    on 2026-11-25 `personal/apfs` still had only milestone 1.
  - `apple-spi-input`: everything before APFS. Tip: `24f9dc8e84` "Merge
    RenkuOS main into apple-spi-input" (RenkuOS `main` at `7b0177e735`).
- Remotes in a working clone:
  - `origin` = https://github.com/RenkuOS/Source.git (read only for us)
  - `personal` = https://github.com/Drake3d/renkuos-macbook.git

### Setting up a new computer

```
git clone https://github.com/RenkuOS/Source.git
cd Source
git remote add personal https://github.com/Drake3d/renkuos-macbook.git
git fetch personal
git switch -c apfs --track personal/apfs
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
```

Already part of every image without a setting: the CS8409 sound (hda),
Kaby Lake brightness (intel_extreme) and the EFI start-up fix. With the
camera files found, a freshly installed image has a working camera
(tested 2026-11-25); without them, copy them by hand (see "Camera").

Configure and build as usual for Haiku (x86_64), in `generated.x86_64`:
`jam -q -j<cores> @nightly-anyboot`, or single targets such as
`facetimehd.hpkg`, `apple_spi_input.hpkg`, `apfs.hpkg`, `apfs`, `hda`.
APFS test shell (runs the driver on macOS against an image file):
`jam -q '<build>apfs_shell'`, then
`objects/darwin/arm64/release/tests/add-ons/kernel/file_systems/apfs/apfs_shell/apfs_shell <image>`
(`APFS_VOLUME=<index|name>` picks a volume).

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
git switch apfs
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
  only, never code; each file names its sources.
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
  preferences app (built, NOT yet tested), package `facetimehd.hpkg`.
- Merged RenkuOS `main` (711 commits) on 2026-11-25; nightly tested on
  the MacBook, including a fresh install with the camera files built in.
- APFS, read-only (branch `apfs`, 5 commits; see apfs-readonly-design.md):
  containers and volumes, folders, files, symlinks, hard links, extended
  attributes, compressed files (LZVN, LZFSE, zlib), programs run from
  APFS, free space as macOS reports it; package `apfs.hpkg` (optional
  image package APFS). Tested on images and on two 4 TB macOS SSDs.

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

## Open items

1. Test the Camera preferences app (Deskbar > Preferences > Camera).
2. Keep camera settings across restarts (they reset on reboot today).
3. Power the camera's processor and sensor down when idle.
4. Camera: sleep and wake; resolutions other than 1280x720.
5. Speakers sometimes silent until a reboot (seen once): on recurrence,
   try Media preferences > Restart media services and save the syslog.
6. Microphone is quiet; optional higher default gain.
7. Wi-Fi (BCM4350): researched only, on hold; see wifi-bcm4350-plan.md.
8. After the merge, check the internal microphone and the input selector
   (upstream reworked hda mux controls) and display moves on Kaby Lake.
9. APFS: names typed in a different Unicode normalization than macOS
   stored (needs NFD and case-folding tables in the kernel); compressed
   files of type 4 (zlib in a classic resource fork, Mac OS X 10.6-10.10,
   refused today); snapshots; FileVault volumes; write support.

## Notes in this branch

- camera-facetimehd-design.md: camera hardware, sources, phase results.
- sound-cs8409-design.md: CS8409 audio design and provenance.
- wifi-bcm4350-plan.md: Wi-Fi plan (on hold).
- apfs-readonly-design.md: APFS driver design, results, and the places
  where real volumes differ from Apple's reference.

## APFS test images

On "NEW FAT VOL" in `apfs-test/` (plain, multi, stress, attrs, comp),
with a README of MacBook test steps. `make_test_images.sh` in
`src/tests/add-ons/kernel/file_systems/apfs/` rebuilds them on a Mac
(comp.dmg by hand, as described there). Attach test images on the Mac
only with `hdiutil attach -readonly`: a normal mount changes them.
