/*
 * Copyright 2026, RenkuOS Contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */
#ifndef APPLE_SPI_PROTOCOL_H
#define APPLE_SPI_PROTOCOL_H


//!	The keyboard and trackpad protocol of the SPI connected input devices
//	in MacBook (2015-2017) and MacBook Pro (2016-2017) machines, ACPI
//	hardware ID APP000D. The layout here was written from the public
//	description of the protocol, as implemented by the Linux applespi
//	driver; no code is taken from it.
//
//	Everything travels in fixed 256 byte packets. A message may span two
//	packets (touchpad reports with many fingers). All values are little
//	endian, and packets and messages each end in a CRC-16/ARC of the bytes
//	before it, so that the CRC over a whole packet or message is zero.


#include <SupportDefs.h>


#define APPLE_SPI_PACKET_SIZE		256
#define APPLE_SPI_PACKET_HEADER		8
#define APPLE_SPI_PACKET_DATA		(APPLE_SPI_PACKET_SIZE \
										- APPLE_SPI_PACKET_HEADER - 2)
#define APPLE_SPI_MAX_MESSAGE		(2 * APPLE_SPI_PACKET_DATA)
#define APPLE_SPI_STATUS_SIZE		4

// packet header
#define APPLE_SPI_PACKET_FLAGS		0
#define APPLE_SPI_PACKET_DEVICE		1
#define APPLE_SPI_PACKET_OFFSET		2		// le16
#define APPLE_SPI_PACKET_REMAINING	4		// le16
#define APPLE_SPI_PACKET_LENGTH		6		// le16

// packet flags
#define APPLE_SPI_READ				0x20	// input from the device
#define APPLE_SPI_WRITE				0x40	// command, or reply to one

// packet device
#define APPLE_SPI_DEVICE_KEYBOARD	0x01
#define APPLE_SPI_DEVICE_TOUCHPAD	0x02
#define APPLE_SPI_DEVICE_INFO		0xd0

// message header, followed by the payload and a CRC
#define APPLE_SPI_MESSAGE_TYPE		0		// le16
#define APPLE_SPI_MESSAGE_ZERO		2
#define APPLE_SPI_MESSAGE_COUNTER	3
#define APPLE_SPI_MESSAGE_RSP_LEN	4		// le16
#define APPLE_SPI_MESSAGE_LENGTH	6		// le16, payload without the CRC
#define APPLE_SPI_MESSAGE_HEADER	8

// commands
#define APPLE_SPI_CMD_TP_INFO		0x1020
#define APPLE_SPI_CMD_MT_INIT		0x0252
#define APPLE_SPI_CMD_CAPS_LOCK		0x0151

#define APPLE_SPI_MT_INIT_MAGIC		0x0102
#define APPLE_SPI_CAPS_LOCK_ON		0x02

// The reply to a write, read right after it.
static const uint8 kAppleSPIStatusOK[APPLE_SPI_STATUS_SIZE]
	= { 0xac, 0x27, 0x68, 0xd5 };

// Delay between the end of a write and reading its status.
#define APPLE_SPI_WRITE_STATUS_DELAY	100

// keyboard report payload
#define APPLE_SPI_KBD_MODIFIERS		1
#define APPLE_SPI_KBD_KEYS			3
#define APPLE_SPI_KBD_KEY_COUNT		6
#define APPLE_SPI_KBD_FN			9
#define APPLE_SPI_KBD_PAYLOAD		10

// touchpad report payload
#define APPLE_SPI_TP_CLICKED		1
#define APPLE_SPI_TP_FINGER_COUNT	30
#define APPLE_SPI_TP_FINGERS		48
#define APPLE_SPI_TP_FINGER_SIZE	30
#define APPLE_SPI_TP_MAX_FINGERS	11

// finger record
#define APPLE_SPI_FINGER_ABS_X		2		// le16, signed
#define APPLE_SPI_FINGER_ABS_Y		4		// le16, signed, grows upwards
#define APPLE_SPI_FINGER_TOUCH_MAJOR 16		// le16, 0 for a lifted finger

// touchpad info reply payload
#define APPLE_SPI_TP_INFO_MODEL		106


static inline uint16
apple_spi_le16(const uint8* data)
{
	return data[0] | (data[1] << 8);
}


static inline void
apple_spi_put_le16(uint8* data, uint16 value)
{
	data[0] = value & 0xff;
	data[1] = value >> 8;
}


//!	CRC-16/ARC: polynomial 0x8005 reflected, initial value 0.
static inline uint16
apple_spi_crc16(const uint8* data, size_t length)
{
	uint16 crc = 0;
	for (size_t i = 0; i < length; i++) {
		crc ^= data[i];
		for (int bit = 0; bit < 8; bit++)
			crc = (crc & 1) != 0 ? (crc >> 1) ^ 0xa001 : crc >> 1;
	}
	return crc;
}


#endif // APPLE_SPI_PROTOCOL_H
