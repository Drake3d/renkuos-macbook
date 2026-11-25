# MacBookPro14,1 Wi-Fi: BCM4350 plan (on hold)

Status: researched only, nothing written. User chose "Not now" on 2026-11-07.

## Hardware (from the MacBook syslog)
- Built-in card: Broadcom **BCM4350** 802.11ac, PCI `14e4:43a3`, revision 05,
  subsystem `106b:0170`, bus 2. FullMAC (the firmware does the 802.11 work).
- Wi-Fi that works today: a USB **RTL8188EU** adapter (`realtekwifi`).
- Also on PCI: FaceTime HD camera `14e4:1570` (separate job).

## Existing code in the tree
- `src/add-ons/kernel/drivers/network/wlan/brcmfmac/` — native RenkuOS driver by
  Bryan Irvine, ISC, following OpenBSD bwfm(4). **SDIO only** (Raspberry Pi 4/400
  CYW43455/43456 on the BCM2711 Arasan controller). Built for arm64 only
  (`build/jam/packages/Haiku`: `brcmfmac@arm64`).
- `Brcmf` class is tied to `SdioHost`. Shareable parts: core/EROM handling,
  passive/active, firmware/NVRAM/CLM loading, dcmd/iovar layer, events,
  scan/join/keys, the net driver interface in `driver.cpp`.
- Firmware is looked up under `<B_SYSTEM_NONPACKAGED_DATA_DIRECTORY or
  B_SYSTEM_DATA_DIRECTORY>/firmware/<subdir>/<name>`.

## Plan (each phase tested on the MacBook)
1. **Bus split + PCIe bring-up**: a bus interface under `Brcmf` (SDIO path kept as
   is; no Pi to test it, compile-check only). PCI attach, BAR0/BAR1 mapping,
   backplane window via PCI config, chip attach, firmware download to RAM,
   firmware start, read the shared-memory info. Done when the syslog shows the
   firmware running.
2. **msgbuf control rings**: the PCIe control protocol (H2D control submit,
   RX post, D2H control/TX/RX complete), DMA buffers, interrupts; ioctls over it.
   Done when the MAC address and firmware version are read.
3. **Data path**: flow rings for TX, RX buffers, hooked to the existing net
   interface.
4. **Scan and join** through the existing logic. Done when Network preferences
   scans and connects.

Size: about 2,500-3,000 new lines; over the 1,000-line rule in AGENTS.md, so
it is split as above.

## Sources and licences
- OpenBSD `sys/dev/pci/if_bwfm_pci.c`, `if_bwfm_pci.h` (ISC), same family the
  existing driver follows. Record with `Ported-from` / `Ported-from-license`.
- Linux brcmfmac (ISC): compare only.
- Firmware: `brcmfmac4350c2-pcie.bin` (or `brcmfmac4350-pcie.bin`, by chip rev)
  from linux-firmware, Broadcom's redistributable closed licence. Not committed;
  the user copies it onto the MacBook. PCIe 4350 should not need an NVRAM file.
