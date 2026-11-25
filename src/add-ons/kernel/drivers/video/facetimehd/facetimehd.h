/*
 * Copyright 2026, RenkuOS Contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */
#ifndef FACETIMEHD_H
#define FACETIMEHD_H


#include <KernelExport.h>
#include <PCI.h>
#include <lock.h>

#include "facetimehd_ioctl.h"


#define FTHD_LOG(x...)		dprintf("facetimehd: " x)

#define FTHD_VENDOR_BROADCOM	0x14e4
#define FTHD_DEVICE_1570		0x1570

// The three memory BARs [F]: the PCIe-to-S2 bridge with its PLLs, DDR
// controller and page table; the camera's own DDR, shared with its
// firmware; and the image processor's registers.
#define FTHD_BAR_S2_IO			0
#define FTHD_BAR_S2_MEM			2
#define FTHD_BAR_ISP_IO			4

// image processor registers [F]
#define ISP_FW_CHAN_CTRL		0xc3000
#define ISP_FW_QUEUE_CTRL		0xc3004
#define ISP_FW_SIZE				0xc3008
#define ISP_FW_HEAP_SIZE		0xc300c
#define ISP_FW_HEAP_ADDR		0xc3010
#define ISP_FW_HEAP_SIZE2		0xc3014
#define ISP_REG_C3018			0xc3018
#define ISP_FW_ARGS				0xc301c
#define ISP_REG_40004			0x40004
#define ISP_REG_40008			0x40008
#define ISP_IRQ_STATUS			0x41000
#define ISP_IRQ_ENABLE			0x41004
#define ISP_DOORBELL			0x41020
#define ISP_IRQ_CLEAR			0x41024

// one interrupt bit per channel source, 0x10 << source [F]
#define ISP_IRQ_CHANNELS		0xf0

#define FTHD_MAX_CHANNELS		32
#define FTHD_MAX_ALLOCATIONS	256
#define FTHD_FRAME_BUFFERS		4


struct fthd_channel {
	char			name[64];
	uint32			type;		// FTHD_CHANNEL_*
	uint32			source;		// interrupt and doorbell bit
	uint32			size;		// entries
	uint32			offset;		// of the ring in S2 MEM
	uint32			index;		// next entry the host looks at
	mutex			lock;
};

// ring direction [F]
enum {
	FTHD_CHANNEL_TO_CAMERA	= 0,
	FTHD_CHANNEL_TO_HOST	= 1,
	FTHD_CHANNEL_TO_HOST_UNI = 2
};

struct fthd_allocation {
	uint32			offset;
	uint32			size;
};

enum {
	FTHD_FRAME_FREE		= 0,	// the driver has it
	FTHD_FRAME_QUEUED,			// the camera has it
	FTHD_FRAME_DONE,			// filled, waiting to be read
	FTHD_FRAME_READING			// being copied to the reader
};

struct fthd_frame {
	uint32			state;
	uint32			descriptor;		// S2 MEM offset of its descriptor list
	uint32			page_index;		// first page table entry
	uint32			sequence;
	bigtime_t		time;
};


struct fthd_device {
	pci_info		pci;
	uint8			pcie_capability;	// 0 if none

	area_id			s2_io_area;
	volatile uint8*	s2_io;
	size_t			s2_io_size;
	area_id			s2_mem_area;
	volatile uint8*	s2_mem;
	size_t			s2_mem_size;
	area_id			isp_io_area;
	volatile uint8*	isp_io;
	size_t			isp_io_size;

	uint32			ddr_speed;			// MHz
	uint32			ddr_model;
	bool			hardware_up;

	// S2 MEM allocations, sorted by offset (memory.cpp)
	mutex			memory_lock;
	fthd_allocation	allocations[FTHD_MAX_ALLOCATIONS];
	int32			allocation_count;

	// firmware and its channels (isp.cpp)
	bool			isp_up;
	fthd_channel	channels[FTHD_MAX_CHANNELS];
	uint32			channel_count;
	fthd_channel*	terminal;
	fthd_channel*	debug;
	fthd_channel*	shared_malloc;
	fthd_channel*	io;
	fthd_channel*	buf_h2t;
	fthd_channel*	buf_t2h;
	fthd_channel*	io_t2h;

	uint32			irq;
	bool			msi;
	bool			irq_installed;
	sem_id			irq_sem;
	thread_id		irq_thread;
	volatile bool	irq_thread_quit;
	int32			interrupts;
	int32			shared_allocations;
	mutex			command_lock;

	// sensor, from the firmware (stream.cpp)
	uint16			sensor_id0;
	uint16			sensor_id1;
	uint32			sensor_count;
	uint32			sensor_width;
	uint32			sensor_height;
	uint32			set_file;			// S2 MEM offset, 0 if not loaded
	int32			controls[FTHD_CONTROL_COUNT];

	// streaming (stream.cpp)
	mutex			stream_lock;
	mutex			frame_lock;
	sem_id			frame_sem;
	bool			streaming;
	uint32			width;
	uint32			height;
	size_t			frame_size;		// page aligned
	area_id			frame_area;
	uint8*			frame_memory;
	uint32			mapped_pages;
	fthd_frame		frames[FTHD_FRAME_BUFFERS];
	uint32			sequence;
	int32			frames_received;
	bigtime_t		stream_start;
};


extern pci_module_info* gPCI;


// #pragma mark - register and memory access


/*!	A configuration space write is not posted, so it pushes the memory
	writes before it out to the device [F].
*/
static inline void
pci_post(fthd_device* device)
{
	gPCI->write_pci_config(device->pci.bus, device->pci.device,
		device->pci.function, 0, 4, 0x12345678);
}


static inline uint32
s2_read(fthd_device* device, uint32 offset)
{
	if (offset + 4 > device->s2_io_size) {
		FTHD_LOG("S2 read beyond the BAR: 0x%" B_PRIx32 "\n", offset);
		return 0;
	}
	return *(volatile uint32*)(device->s2_io + offset);
}


static inline void
s2_write(fthd_device* device, uint32 offset, uint32 value)
{
	if (offset + 4 > device->s2_io_size) {
		FTHD_LOG("S2 write beyond the BAR: 0x%" B_PRIx32 "\n", offset);
		return;
	}
	*(volatile uint32*)(device->s2_io + offset) = value;
	pci_post(device);
}


static inline uint32
isp_read(fthd_device* device, uint32 offset)
{
	if (offset + 4 > device->isp_io_size) {
		FTHD_LOG("ISP read beyond the BAR: 0x%" B_PRIx32 "\n", offset);
		return 0;
	}
	return *(volatile uint32*)(device->isp_io + offset);
}


static inline void
isp_write(fthd_device* device, uint32 offset, uint32 value)
{
	if (offset + 4 > device->isp_io_size) {
		FTHD_LOG("ISP write beyond the BAR: 0x%" B_PRIx32 "\n", offset);
		return;
	}
	*(volatile uint32*)(device->isp_io + offset) = value;
	pci_post(device);
}


static inline uint32
mem_read(fthd_device* device, uint32 offset)
{
	if (offset + 4 > device->s2_mem_size) {
		FTHD_LOG("S2 MEM read beyond the BAR: 0x%" B_PRIx32 "\n", offset);
		return 0;
	}
	return *(volatile uint32*)(device->s2_mem + offset);
}


static inline void
mem_write(fthd_device* device, uint32 offset, uint32 value)
{
	if (offset + 4 > device->s2_mem_size) {
		FTHD_LOG("S2 MEM write beyond the BAR: 0x%" B_PRIx32 "\n", offset);
		return;
	}
	*(volatile uint32*)(device->s2_mem + offset) = value;
}


// hardware.cpp
status_t	fthd_map(fthd_device* device);
void		fthd_unmap(fthd_device* device);
status_t	fthd_hardware_init(fthd_device* device);

// memory.cpp
void		fthd_memory_init(fthd_device* device);
void		fthd_memory_uninit(fthd_device* device);
status_t	fthd_memory_reserve(fthd_device* device, uint32 offset,
				uint32 size);
status_t	fthd_memory_alloc(fthd_device* device, uint32 size,
				uint32* _offset);
void		fthd_memory_free(fthd_device* device, uint32 offset);
void		fthd_memory_copy_to(fthd_device* device, uint32 offset,
				const void* buffer, size_t length);
void		fthd_memory_copy_from(fthd_device* device, uint32 offset,
				void* buffer, size_t length);

// isp.cpp
status_t	fthd_isp_init(fthd_device* device);
void		fthd_isp_uninit(fthd_device* device);
status_t	fthd_isp_command(fthd_device* device, uint16 opcode,
				const void* request, size_t requestLength, void* response,
				size_t responseLength);
bool		fthd_ring_host_owns(fthd_device* device, fthd_channel* channel,
				uint32 entry);
status_t	fthd_ring_send(fthd_device* device, fthd_channel* channel,
				uint32 address, uint32 requestSize, uint32 responseSize,
				uint32* _entry);
int			fthd_open_firmware(const char* name, char* path,
				size_t pathSize);
status_t	fthd_copy_file_to_memory(fthd_device* device, int fd,
				const char* path, uint32 offset, size_t maxSize,
				size_t* _size);

// stream.cpp
void		fthd_stream_init(fthd_device* device);
void		fthd_stream_uninit(fthd_device* device);
void		fthd_stream_query_sensor(fthd_device* device);
status_t	fthd_stream_start(fthd_device* device);
void		fthd_stream_stop(fthd_device* device);
status_t	fthd_stream_set_control(fthd_device* device, uint32 id,
				int32 value);
status_t	fthd_stream_read_frame(fthd_device* device, void* buffer,
				size_t bufferSize, bigtime_t timeout, uint32* _sequence,
				bigtime_t* _time);
void		fthd_stream_frame_returned(fthd_device* device,
				uint32 descriptorList);


#endif	// FACETIMEHD_H
