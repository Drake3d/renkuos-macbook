# RenkuOS on the MacBookPro14,1: handoff

Last updated 2026-11-25. Read this first when continuing on another
computer. This branch (`notes`) holds only notes; the code is on the
`apple-spi-input` branch of this repository.

## Where things are

- Code: github.com/Drake3d/renkuos-macbook, branch `apple-spi-input`
  (based on RenkuOS/Source `main`). Tip: `24f9dc8e84` "Merge RenkuOS main
  into apple-spi-input" (RenkuOS `main` at `7b0177e735`, 2026-11-25).
- Remotes in a working clone:
  - `origin` = https://github.com/RenkuOS/Source.git (read only for us)
  - `personal` = https://github.com/Drake3d/renkuos-macbook.git

### Setting up a new computer

```
git clone https://github.com/RenkuOS/Source.git
cd Source
git remote add personal https://github.com/Drake3d/renkuos-macbook.git
git fetch personal
git switch -c apple-spi-input --track personal/apple-spi-input
```

Then create `build/jam/UserBuildConfig` (git-ignored):

```
# Personal build settings (git-ignored).
AddOptionalHaikuImagePackages AppleSPIInput ;
AddOptionalHaikuImagePackages FaceTimeHD ;

# Personal test images only: put Apple's camera files into the image.
# Adjust the folder to wherever the two files are on this computer.
local fthdWorking = /Volumes/Source/working ;
AddFilesToHaikuImage system non-packaged data firmware facetimehd
	: $(fthdWorking)/fthd-extract/firmware.bin
	  $(fthdWorking)/setfile-extract/1571_01XX.dat ;
```

The last block is optional; without it, copy the camera files by hand
(see "Camera" below). With it, a freshly flashed or installed image has a
working camera (tested 2026-11-25). The files stay outside the repository.

Configure and build as usual for Haiku (x86_64), in `generated.x86_64`:
`jam -q -j<cores> @nightly-anyboot`, or single targets such as
`facetimehd.hpkg`, `apple_spi_input.hpkg`, `hda`.

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
git switch apple-spi-input
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

## Notes in this branch

- camera-facetimehd-design.md: camera hardware, sources, phase results.
- sound-cs8409-design.md: CS8409 audio design and provenance.
- wifi-bcm4350-plan.md: Wi-Fi plan (on hold).
