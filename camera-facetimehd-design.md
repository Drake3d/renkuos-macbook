# MacBookPro14,1 camera: FaceTime HD (14e4:1570) design and provenance

Status: phase 1 passed on the MacBookPro14,1 (2026-11-07): link, PLL (24 MHz ref, locked in 42 us), DDR model 4 at 450 MHz, memory check over all 256 MB. Note: BAR 2 is 256 MB on this model, not 128 MB.
Phase 2 passed too (2026-11-07): firmware S2ISP-01.57.00 boots, 7 channels, MSI interrupt 29 works, CMD_START and PRINT_ENABLE answer with status 0, firmware log text arrives. CMPE on \\_SB.PCI0.RP05 takes no argument on this model.
Phase 3 passed (2026-11-07): sensor 0005 0248 (set file 1571_01XX.dat), native 1296x736, module serial reported; all 22 stream commands status 0; first frame after 347 ms; 64 frames in 2.39 s incl. start-up; facetimehd_capture saved a correct 1280x720 picture with a slight green cast (no set file yet).
Phase 4 passed (2026-11-07): facetimehd.media_addon is the default video input; CodyCam shows live video (320x240 RGB32), 536 frames in 18.1 s (29.5 fps); camera turns off when CodyCam closes (it only disconnects, so the add-on stops on disconnect). The node must call AddNodeKind(B_PHYSICAL_INPUT) itself. Same approach as the
sound driver: learn from the GPL Linux driver, write new MIT code, record where
every fact came from.

## 1. Hardware (from the MacBook syslog, verified)
- PCI bus 3, `14e4:1570` "720p FaceTime HD Camera", class 04/80 (multimedia),
  subsystem `14e4:1570`, interrupt pin A.
- Nothing in the RenkuOS tree drives it. Haiku's `usb_webcam` media add-on is
  built around USB cameras and does not fit a PCIe device.

## 2. Sources
- **[F]** patjak/facetimehd (GPL-2.0-only), reverse engineered Linux driver:
  `fthd_drv.c`, `fthd_hw.c`, `fthd_ddr.c`, `fthd_isp.c/.h`, `fthd_ringbuf.c`,
  `fthd_buffer.c`, `fthd_v4l2.c`, `fthd_reg.h`. About 4,500 lines. Facts only,
  no code.
- **[W]** its wiki: PCIe interface, ring buffer channels, set files.
- **[X]** patjak/facetimehd-firmware (GPL-2.0): how the firmware is extracted.
  We do not ship the tool or the firmware.

## 3. How the hardware works [F][W]
### PCI
- BAR 0 "S2 IO": PCIe-to-S2 link registers (0xd000-0xd12c), PLL and DDR
  controller, and a page table ("IOMMU") at 0x9000, 0x1000 four-byte entries.
- BAR 2 "S2 MEM": memory shared with the firmware, 128 MB (the camera's own DDR).
  The first 8 MB hold the firmware; the rest is a host-managed heap for rings,
  commands and descriptors.
- BAR 4 "ISP IO": ISP control registers (1 MB).
- MSI interrupt; bus master; 64-bit DMA, falling back to 32-bit.
- ASPM (L0s, L1, clock PM) must be off, or the device hangs while streaming.
- PCI config 0x94 = 0x200 enables interrupts; 0x90 = 0x200 acknowledges.

### Bring-up order
1. Clear the page table.
2. PCIe link set-up writes in BAR 0 (0xd000.., with a read-back check of 0xd124).
3. DDR: PLL (24 or 25 MHz reference, 450 MHz), PHY reset, pad and PHY PLLs,
   mode and strap registers, VDL calibration, VTT/ZQ, mode registers. Some
   values depend on a "model" number. *Verify which applies to the 14,1.*
4. Memory check: a pseudo-random pattern written to and read back from S2 MEM.
5. ISP: copy `firmware.bin` into the first 8 MB, power the sensor on via the
   ACPI method `CMPE(1)` on the camera's ACPI node, clear the firmware registers
   (0xc3000-0xc301c), start the ISP (0x40008, 0x40004), and wait for the wake
   interrupt (status 0x41000, bits 0xf0).
6. Give the firmware its heap and arguments, ring 0x41020, wait for the second
   wake, then read the channel table: 256-byte records (name, type, source,
   size, offset). Seven channels: TERMINAL, DEBUG, SHAREDMALLOC, IO, BUF_H2T,
   BUF_T2H, IO_T2H.
7. Handshake: write 0x8042006 to 0xc300c and wait for 0.

### Rings and commands
- 64-byte entries: address and owner bit, request size, response size.
- Doorbell: `0x10 << source` to 0x41020. Interrupt status 0x41000, clear 0x41024,
  enable 0x41004 = 0xf8.
- Commands go on IO: an 8-byte header (opcode, status) and a payload in S2 MEM.
  The stream start uses about 20 commands (camera config, crop, output
  config YUYV, auto exposure, DRC, temporal filter, CH_START 0x100).
- SHAREDMALLOC: the firmware asks the host for memory; IO_T2H only needs acks.
- TERMINAL carries firmware log text, useful for the syslog.

### Frames
- YUYV (= Haiku `B_YCbCr422`), up to the sensor size, 1280x720, 30 fps.
- Each frame buffer is mapped through the page table and described by a
  0x180-byte descriptor sent on BUF_H2T. Filled frames come back on BUF_T2H.

### Firmware and calibration
- `firmware.bin`: Apple's binary, from the AppleCameraInterface driver in the
  macOS 10.12.6 combo update (version 5.60.0), gzip at offset 81920, 602,903
  bytes compressed; sha256 `240ef2e9…4330` [X]. Not redistributable. The user
  extracts it and copies it to the MacBook.
- MacBookPro14,1 sensor reports `0005 0248` and uses the set file
  `1571_01XX.dat` [W]. Optional: without it, colours are off.
- [F] has no confirmed report for the MacBookPro14,1, and one open request
  (#314) about firmware for this model. **This is the biggest risk.**

## 4. Design for RenkuOS
- **Kernel driver** `drivers/video/facetimehd` (new, MIT): PCI attach, bring-up,
  rings, commands and frame buffers. Publishes `/dev/video/facetimehd/0` with a
  small ioctl interface: start, stop, and wait for the next frame into a buffer
  the caller maps.
- **Media add-on** `facetimehd` (new, MIT): a `BBufferProducer` that outputs
  `B_YCbCr422` 1280x720 frames, so Haiku camera apps can use it.
- Both ship in a package of their own, like `apple_spi_input`, plus the nightly.

## 5. Phases
| Phase | Goal | Done when |
|---|---|---|
| 1 | PCI attach, link, DDR, memory check | syslog: memory check passes |
| 2 | Firmware load, ISP start, channel table, handshake | syslog: seven channels and firmware log text |
| 3 | Commands, sensor info, stream start, frames into a buffer | syslog: frames arrive; a test tool saves one frame |
| 4 | Media add-on | a camera app shows live video |
| 5 | Set file, controls, power-down | correct colours; camera off when idle |
