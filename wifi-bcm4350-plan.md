# MacBookPro14,1 Wi-Fi: BCM4350 plan (in progress)

Status: phase 1 done and tested on the MacBook 2026-11-26 (not committed yet).
Chip BCM4350 rev 5, PCIe core rev 11, CR4 rev 6, RAM 768 KB at 0x180000;
firmware brcmfmac4350c2-pcie.bin (7.35.180.133) copied in 364 ms, running
after 515 ms; shared area version 5, flags 0x20005 (no DMA indices), max
255 RX buffers, firmware console readable. Code: brcmf_chip.cpp,
brcmf_pcie.cpp, PCI attach in driver.cpp, package BroadcomWifi.
Phase 2 done and tested 2026-11-26 (not committed): brcmf_pcie_msgbuf.cpp,
five common rings in host memory, 42 submission / 40 flow / 3 completion
rings, MSI interrupt (vector 27), cur_etheraddr and "ver" iovars answered
in about 1.3 ms, 2 interrupts and nothing found by polling.
Committed as 6d6e54bbb9 on branch wifi (phases 1+2).
Phase 3 tested 2026-11-26, committed as 10961cdba7: Brcmf runs over BrcmfTransport;
brcmf_pcie_data.cpp (255 RX buffers, one flow ring, TX copy buffers).
Joined a WPA2-PSK/CCMP network with the host supplicant (wpa_supplicant),
keys installed, DHCP address, ping 12-32 ms, 0% loss. "ifconfig list"
was empty because nothing had started a scan ("ifconfig scan" not tried
yet).
Open items fixed and tested 2026-11-26, committed as c263cd2c11: flow ring per
FIFO from the IP precedence; flow rings deleted on link down and
reopened by the next frame; SSID-only scans no longer prune the list;
SDIO chip code moved onto brcmf_chip.cpp (CM3/SOCRAM added there;
compile-checked only, no Pi). Scan listed 18 networks, list kept them;
"leave" dropped the link, net_server rejoined after 13 s on another
access point of the same mesh (6a:d2:ff:93:d0:62), keys, DHCP, ping OK.

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

## Test rounds (2026-11-26, committed as 419c2da7d4)
- Round 1: Network preferences works; 5 GHz channel 159 at 400 Mbit/s;
  50 MB upload in 15 s (about 27 Mbit/s), no TX failures; idle 15 min OK.
- Round 2: signal fixed (-29 dBm, from the scan when the firmware has no
  reading yet); upload again ~28 Mbit/s. Downloads "failed", but only
  because the test servers did: Cloudflare's __down sent 1 byte to curl,
  and speedtest.tele2.net never answered on IPv4 port 80.
- Round 3: pings up to 1472 bytes (and 3000, fragmented) answered, to the
  internet and the router; web pages load; up to 5730 frames of 1514
  bytes a minute received. Large frames are fine.
- Download: linux-6.6.tar.xz from cdn.kernel.org (about 140 MB) arrived
  within one minute's summary: 95467 frames, 93958 of them full size,
  18.9 Mbit/s averaged over that minute (so at least that), no drops,
  all 255 RX buffers kept posted.

## Other architectures (2026-11-26)
- arm64: the whole driver (SDIO and PCIe sides) builds with no warnings;
  all 64 kernel symbols it uses are in the arm64 kernel.so. Not run on a
  Raspberry Pi yet.
- 32-bit x86: all 8 sources compile with GCC 13 for i586 with the kernel
  flags, no warnings. Not linked (plain x86 has no package snapshot) and
  not tried with GCC 2.95, which the x86_gcc2h hybrid may use for kernel
  drivers. Details in HANDOFF.md, "Other architectures".
