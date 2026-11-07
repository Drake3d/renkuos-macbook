/*
 * Copyright 2026, RenkuOS Contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */
#ifndef APPLE_SPI_TOUCHPAD_H
#define APPLE_SPI_TOUCHPAD_H


#include <keyboard_mouse_driver.h>
#include <lock.h>

#include "AppleSPIProtocol.h"


#define APPLE_SPI_TP_QUEUE		64


struct apple_finger {
	int32	x;
	int32	y;
};


//!	The trackpad, published as a touchpad: input_server's touchpad code
//	(movement_maker) does pointer motion, tapping and two finger scrolling
//	from the positions reported here, as it does for Synaptics and Elan
//	touchpads. The pad has one button under it; right and middle clicks are
//	made from the number of fingers on it.
class AppleSPITouchpad {
public:
								AppleSPITouchpad();
								~AppleSPITouchpad();

			status_t			InitCheck() const { return fStatus; }

			bool				IsOpen() const { return fOpenCount > 0; }
			status_t			Open(uint32 flags);
			status_t			Close();

			void				Removed();
			bool				IsRemoved() const { return fRemoved; }

			void				SetPublishPath(char* publishPath);
			const char*			PublishPath() const { return fPublishPath; }

			status_t			Control(uint32 op, void* buffer, size_t length);

			// from the transport
			void				SetModel(uint8 model);
			void				HandleReport(const uint8* payload,
									size_t length);

private:
			int					_ParseReport(const uint8* payload,
									size_t length, touchpad_movement* movement);
			void				_TrackPosition(touchpad_movement* movement,
									int count);
			int					_ClickFingers(int count,
									const apple_finger** presser) const;
			bool				_EmulateButtons(touchpad_movement* movement,
									int count);
			void				_ResetContact();

			void				_Queue(const touchpad_movement& movement);
			status_t			_Dequeue(touchpad_movement* movement,
									bigtime_t timeout);

private:
			status_t			fStatus;
			int32				fOpenCount;
			bool				fRemoved;
			char*				fPublishPath;

			// device coordinates of the pad's corners
			int32				fMinX;
			int32				fMinY;
			int32				fMaxX;
			int32				fMaxY;
			uint32				fWidth;			// after dividing by fScale
			uint32				fHeight;
			int32				fScale;
			touchpad_specs		fHardwareSpecs;

			apple_finger		fFingers[APPLE_SPI_TP_MAX_FINGERS];
			apple_finger		fLastFingers[APPLE_SPI_TP_MAX_FINGERS];
			int					fCount;
			int					fLastFingerCount;
			int32				fPositionX;
			int32				fPositionY;

			// clickpad button emulation
			uint32				fClickButton;
			int					fLastCount;
			int					fContactMaxFingers;
			bigtime_t			fContactStart;
			uint32				fTapX;
			uint32				fTapY;
			bool				fContactMoved;
			bool				fContactClicked;
			bool				fIgnoreContact;
			bool				fPendingRelease;

			// events for MS_READ_TOUCHPAD
			mutex				fQueueLock;
			sem_id				fQueueSem;
			touchpad_movement	fQueue[APPLE_SPI_TP_QUEUE];
			uint32				fQueueHead;
			uint32				fQueueCount;

			int32				fLoggedProblems;	// first few, for debugging
};


#endif // APPLE_SPI_TOUCHPAD_H
