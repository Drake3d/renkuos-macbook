/*
 * Copyright 2026, RenkuOS Contributors
 * All rights reserved. Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 */

/*
 * The link between the Wi-Fi logic of the brcmfmac driver (class Brcmf:
 * firmware set-up, scanning, joining, keys, events) and a bus that carries
 * firmware commands and frames in its own way. The SDIO chips are driven by
 * Brcmf itself (SDPCM/BCDC); the PCIe chips use the msgbuf protocol in
 * BrcmfPcie, which plugs in here.
 */
#ifndef BRCMF_TRANSPORT_H
#define BRCMF_TRANSPORT_H


#include <OS.h>

#include <net_buffer.h>


class BrcmfTransport {
public:
	virtual						~BrcmfTransport() {}

	// Sends a firmware command (dcmd). \a data holds \a length bytes; for
	// a get, it receives the reply, whose length goes to *_replyLength.
	virtual	status_t			TransportDcmd(uint32 command, bool set,
									void* data, size_t length,
									size_t* _replyLength) = 0;

	// Sends an Ethernet frame. The buffer is freed on success only.
	virtual	status_t			TransportSend(net_buffer* buffer) = 0;
};


class BrcmfReceiver {
public:
	virtual						~BrcmfReceiver() {}

	// A firmware event: an Ethernet frame of type 0x886c.
	virtual	void				ReceiveEvent(const uint8* data,
									size_t length) = 0;
	// A received Ethernet frame.
	virtual	void				ReceiveData(const uint8* data,
									size_t length) = 0;
};


#endif	// BRCMF_TRANSPORT_H
