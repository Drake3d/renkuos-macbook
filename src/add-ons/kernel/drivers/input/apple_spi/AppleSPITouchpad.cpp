/*
 * Copyright 2026, RenkuOS Contributors.
 * Copyright 2023 Vladimir Serbinenko <phcoder@gmail.com>
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


//!	Trackpad half of the Apple SPI driver. The position tracking and the
//	clickpad button emulation follow the I2C Elan touchpad driver
//	(i2c_elan/ELANDevice.cpp), adapted to Apple reports, which give fingers
//	in no fixed order.


#include "AppleSPITouchpad.h"

#include <stdlib.h>
#include <string.h>

#include <kernel.h>
#include <util/AutoLock.h>

#include "Driver.h"


// A touch of two or more fingers that ends within this time, without moving
// or pressing the pad, is a tap for the right (or middle) button.
static const bigtime_t kMultiFingerTapTimeout = 250000;

#define BUTTON_LEFT		0x01
#define BUTTON_RIGHT	0x02
#define BUTTON_MIDDLE	0x04

// Contacts get a fixed pressure: the reports have no value movement_maker
// could use directly. It is above the touch threshold, below maxPressure.
#define CONTACT_PRESSURE	40

// movement_maker is calibrated on a Synaptics pad 4100 units wide and keeps
// its speed as 4100 / width in an int8: a wider pad gives 0, and the
// input_server mouse add-on divides by it. Apple pads are over 12000 units
// wide, so positions are divided down to at most this width.
#define MOVEMENT_MAKER_WIDTH	4100

// How many dropped reports to log, for debugging.
static const int32 kLoggedProblems = 8;


struct apple_touchpad_model {
	uint8	model;
	int16	minX;
	int16	minY;
	int16	maxX;
	int16	maxY;
};

// Size of the pad in device units, by the model number the pad reports.
// Values as used by the Linux applespi driver; unconfirmed here.
static const apple_touchpad_model kTouchpadModels[] = {
	{ 0x04, -5087, -182, 5579, 6089 },	// MacBook8,1 9,1 10,1
	{ 0x05, -6243, -170, 6749, 7685 },	// MacBookPro13,1 13,2 14,1 14,2
	{ 0x06, -7456, -163, 7976, 9283 },	// MacBookPro13,3 14,3
};

// The 13 inch MacBook Pro, until the pad says what it is.
static const int kDefaultModel = 1;


AppleSPITouchpad::AppleSPITouchpad()
	:
	fStatus(B_NO_INIT),
	fOpenCount(0),
	fRemoved(false),
	fPublishPath(NULL),
	fCount(0),
	fLastFingerCount(0),
	fPositionX(0),
	fPositionY(0),
	fQueueHead(0),
	fQueueCount(0),
	fLoggedProblems(0)
{
	mutex_init(&fQueueLock, "apple_spi touchpad queue");
	_ResetContact();
	memset(fFingers, 0, sizeof(fFingers));
	memset(fLastFingers, 0, sizeof(fLastFingers));

	const apple_touchpad_model& model = kTouchpadModels[kDefaultModel];
	SetModel(model.model);

	fQueueSem = create_sem(0, "apple_spi touchpad events");
	fStatus = fQueueSem >= 0 ? B_OK : fQueueSem;
}


AppleSPITouchpad::~AppleSPITouchpad()
{
	delete_sem(fQueueSem);
	mutex_destroy(&fQueueLock);
	free(fPublishPath);
}


status_t
AppleSPITouchpad::Open(uint32 flags)
{
	if (atomic_add(&fOpenCount, 1) == 0)
		TRACE_ALWAYS("trackpad opened\n");
	return B_OK;
}


status_t
AppleSPITouchpad::Close()
{
	if (atomic_add(&fOpenCount, -1) == 1) {
		MutexLocker locker(fQueueLock);
		if (fQueueCount > 0)
			acquire_sem_etc(fQueueSem, fQueueCount, B_RELATIVE_TIMEOUT, 0);
		fQueueCount = 0;
	}
	return B_OK;
}


void
AppleSPITouchpad::Removed()
{
	fRemoved = true;
	// wake up a reader, it will see the flag
	release_sem_etc(fQueueSem, 1, B_DO_NOT_RESCHEDULE);
}


void
AppleSPITouchpad::SetPublishPath(char* publishPath)
{
	free(fPublishPath);
	fPublishPath = publishPath;
}


void
AppleSPITouchpad::SetModel(uint8 modelNumber)
{
	const apple_touchpad_model* model = NULL;
	for (size_t i = 0; i < B_COUNT_OF(kTouchpadModels); i++) {
		if (kTouchpadModels[i].model == modelNumber)
			model = &kTouchpadModels[i];
	}
	if (model == NULL) {
		TRACE_ALWAYS("unknown touchpad model 0x%02x, using the 13 inch "
			"MacBook Pro size\n", modelNumber);
		model = &kTouchpadModels[kDefaultModel];
	}

	fMinX = model->minX;
	fMinY = model->minY;
	fMaxX = model->maxX;
	fMaxY = model->maxY;
	uint32 width = fMaxX - fMinX;
	fScale = max_c((width + MOVEMENT_MAKER_WIDTH - 1) / MOVEMENT_MAKER_WIDTH,
		1);
	fWidth = width / fScale;
	fHeight = (fMaxY - fMinY) / fScale;

	memset(&fHardwareSpecs, 0, sizeof(fHardwareSpecs));
	fHardwareSpecs.areaStartX = 0;
	fHardwareSpecs.areaEndX = fWidth;
	fHardwareSpecs.areaStartY = 0;
	fHardwareSpecs.areaEndY = fHeight;
	// The Synaptics driver uses 55 for a pad about 4000 units wide.
	fHardwareSpecs.edgeMotionWidth = max_c(fWidth * 55 / 4000, 1);
	fHardwareSpecs.minPressure = 0;
	fHardwareSpecs.realMaxPressure = 50;
	fHardwareSpecs.maxPressure = 255;

	TRACE_ALWAYS("touchpad model 0x%02x, %" B_PRId32 "x%" B_PRId32 ", reported "
		"as %" B_PRIu32 "x%" B_PRIu32 "\n", model->model, fMaxX - fMinX,
		fMaxY - fMinY, fWidth, fHeight);
}


status_t
AppleSPITouchpad::Control(uint32 op, void* buffer, size_t length)
{
	switch (op) {
		case B_GET_DEVICE_NAME:
		{
			if (!IS_USER_ADDRESS(buffer))
				return B_BAD_ADDRESS;
			if (user_strlcpy((char*)buffer, DEVICE_NAME " Trackpad", length)
					> 0) {
				return B_OK;
			}
			return B_ERROR;
		}

		case MS_IS_TOUCHPAD:
			if (buffer == NULL)
				return B_OK;
			if (!IS_USER_ADDRESS(buffer))
				return B_BAD_ADDRESS;
			return user_memcpy(buffer, &fHardwareSpecs,
				sizeof(fHardwareSpecs));

		case MS_READ_TOUCHPAD:
		{
			touchpad_read read;
			if (length < sizeof(touchpad_read))
				return B_BUFFER_OVERFLOW;
			if (!IS_USER_ADDRESS(buffer)
				|| user_memcpy(&read.timeout,
					&((touchpad_read*)buffer)->timeout,
					sizeof(bigtime_t)) != B_OK) {
				return B_BAD_ADDRESS;
			}

			read.event = MS_READ_TOUCHPAD;
			status_t status = _Dequeue(&read.u.touchpad, read.timeout);
			if (status != B_OK)
				return status;

			if (user_memcpy(buffer, &read, sizeof(read)) != B_OK)
				return B_BAD_ADDRESS;
			return B_OK;
		}
	}

	return B_DEV_INVALID_IOCTL;
}


//!	Takes a touchpad report payload from the transport thread.
void
AppleSPITouchpad::HandleReport(const uint8* payload, size_t length)
{
	if (fRemoved)
		return;

	if (!IsOpen()) {
		if (fLoggedProblems < kLoggedProblems) {
			fLoggedProblems++;
			TRACE_ALWAYS("trackpad report while nobody has it open\n");
		}
		// Forget the touch in progress; whoever opens next starts fresh.
		_ResetContact();
		fLastFingerCount = 0;
		return;
	}

	touchpad_movement movement;
	int count = _ParseReport(payload, length, &movement);
	if (count < 0)
		return;

	_TrackPosition(&movement, count);

	if (!_EmulateButtons(&movement, count))
		return;

	_Queue(movement);

	if (fPendingRelease) {
		// _EmulateButtons() reported a tap; release the button right after.
		fPendingRelease = false;
		touchpad_movement release;
		memset(&release, 0, sizeof(release));
		release.fingerWidth = 4;
		_Queue(release);
	}
}


int
AppleSPITouchpad::_ParseReport(const uint8* payload, size_t length,
	touchpad_movement* movement)
{
	if (length < APPLE_SPI_TP_FINGERS) {
		if (fLoggedProblems < kLoggedProblems) {
			fLoggedProblems++;
			TRACE_ALWAYS("short trackpad report, %" B_PRIuSIZE " bytes\n",
				length);
		}
		return -1;
	}

	// The last two bytes of the last finger record are the message CRC,
	// which the transport has already taken off. Nothing used here is
	// stored in them.
	int reported = payload[APPLE_SPI_TP_FINGER_COUNT];
	if (reported > APPLE_SPI_TP_MAX_FINGERS
		|| (reported > 0 && length < APPLE_SPI_TP_FINGERS
			+ (size_t)reported * APPLE_SPI_TP_FINGER_SIZE - 2)) {
		if (fLoggedProblems < kLoggedProblems) {
			fLoggedProblems++;
			TRACE_ALWAYS("bad trackpad report: %d fingers in %" B_PRIuSIZE
				" bytes\n", reported, length);
		}
		return -1;
	}

	memset(movement, 0, sizeof(*movement));
	if (payload[APPLE_SPI_TP_CLICKED] != 0)
		movement->buttons = BUTTON_LEFT;

	int count = 0;
	for (int i = 0; i < reported; i++) {
		const uint8* finger = payload + APPLE_SPI_TP_FINGERS
			+ i * APPLE_SPI_TP_FINGER_SIZE;

		// A finger that is lifted stays in the list for one more report.
		if (apple_spi_le16(finger + APPLE_SPI_FINGER_TOUCH_MAJOR) == 0)
			continue;

		int32 x = (int16)apple_spi_le16(finger + APPLE_SPI_FINGER_ABS_X);
		int32 y = (int16)apple_spi_le16(finger + APPLE_SPI_FINGER_ABS_Y);
		fFingers[count].x = min_c(max_c(x - fMinX, 0) / fScale,
			(int32)fWidth);
		fFingers[count].y = min_c(max_c(y - fMinY, 0) / fScale,
			(int32)fHeight);
		count++;
	}

	fCount = count;
	return count;
}


/*!	Reports one position for all fingers. It follows the finger that moved
	most since the last report, matched by distance, so that adding or
	lifting a finger does not make the position jump: movement_maker would
	move the pointer by that much, at the end of every two finger scroll.
*/
void
AppleSPITouchpad::_TrackPosition(touchpad_movement* movement, int count)
{
	int64 sumX = 0, sumY = 0;
	int32 deltaX = 0, deltaY = 0, moved = -1;
	// A finger that moved more than this since the last report is taken
	// for another finger.
	int64 maxJump = fWidth / 8;
	maxJump *= maxJump;

	for (int i = 0; i < count; i++) {
		sumX += fFingers[i].x;
		sumY += fFingers[i].y;

		int best = -1;
		int64 bestDistance = maxJump;
		for (int j = 0; j < fLastFingerCount; j++) {
			int64 dx = fFingers[i].x - fLastFingers[j].x;
			int64 dy = fFingers[i].y - fLastFingers[j].y;
			int64 distance = dx * dx + dy * dy;
			if (distance <= bestDistance) {
				bestDistance = distance;
				best = j;
			}
		}
		if (best < 0)
			continue;

		// Follow the finger that moved most, so that a thumb resting on
		// the pad does not slow down the finger dragging next to it.
		int32 dx = fFingers[i].x - fLastFingers[best].x;
		int32 dy = fFingers[i].y - fLastFingers[best].y;
		if (abs(dx) + abs(dy) > moved) {
			moved = abs(dx) + abs(dy);
			deltaX = dx;
			deltaY = dy;
		}
	}

	if (count > 0 && fLastFingerCount == 0) {
		fPositionX = sumX / count;
		fPositionY = sumY / count;
	} else {
		// When no finger matches the last report, there is nothing to
		// follow; hold the position.
		fPositionX = min_c(max_c(fPositionX + deltaX, 0), (int32)fWidth);
		fPositionY = min_c(max_c(fPositionY + deltaY, 0), (int32)fHeight);
	}

	memcpy(fLastFingers, fFingers, sizeof(fLastFingers));
	fLastFingerCount = count;

	movement->fingers = count > 0 ? (1 << min_c(count, 5)) - 1 : 0;
	if (count == 0) {
		// A width of 0 or 1 would read as two or three fingers.
		movement->fingerWidth = 4;
		return;
	}

	// movement_maker reads 0, 0 as no touch.
	movement->xPosition = max_c(fPositionX, 1);
	movement->yPosition = fPositionY;
	movement->zPressure = CONTACT_PRESSURE;
	// movement_maker follows the Synaptics width scale: 4 to 7 is a
	// finger, 0 and 1 mean two and three fingers.
	if (count == 1)
		movement->fingerWidth = 4;
	else
		movement->fingerWidth = count == 2 ? 0 : 1;
}


/*!	Counts the fingers of a click. A thumb pressing the bottom of the pad
	while another finger rests higher up is a one finger click, so that a
	thumb can click and the other finger drag. Returns the finger that
	presses the pad: the lowest.
*/
int
AppleSPITouchpad::_ClickFingers(int count, const apple_finger** presser) const
{
	const apple_finger* lowest = NULL;
	int low = 0;
	for (int i = 0; i < count; i++) {
		// Y grows upwards, as movement_maker expects.
		if (lowest == NULL || fFingers[i].y < lowest->y)
			lowest = &fFingers[i];
		if (fFingers[i].y < (int32)fHeight / 5)
			low++;
	}

	*presser = lowest;
	if (count == 2 && low == 1)
		return 1;
	return count;
}


//!	Turns the single button under the pad into right and middle buttons too.
bool
AppleSPITouchpad::_EmulateButtons(touchpad_movement* movement, int count)
{
	bigtime_t now = system_time();

	if (fIgnoreContact) {
		// The rest of a touch that was already reported as a tap.
		if (count == 0)
			_ResetContact();
		return false;
	}

	if (count > 0 && fLastCount == 0) {
		fContactStart = now;
		fContactMaxFingers = 0;
		fContactMoved = false;
		fContactClicked = false;
	}
	fLastCount = count;

	if (count >= 2) {
		uint32 slop = max_c(fWidth / 30, 1);
		if (fContactMaxFingers < 2) {
			fTapX = movement->xPosition;
			fTapY = movement->yPosition;
		} else if (abs((int32)movement->xPosition - (int32)fTapX) > (int32)slop
			|| abs((int32)movement->yPosition - (int32)fTapY) > (int32)slop) {
			fContactMoved = true;
		}
	}
	fContactMaxFingers = max_c(fContactMaxFingers, count);

	// The pad clicks as one button. A click with two fingers, or with one
	// finger in the bottom right corner, is a right click, and one with
	// three fingers a middle click. The button is chosen when the pad goes
	// down and kept until it comes up again.
	if ((movement->buttons & BUTTON_LEFT) != 0) {
		fContactClicked = true;
		if (fClickButton == 0) {
			const apple_finger* presser;
			int fingers = _ClickFingers(count, &presser);
			if (fingers >= 3)
				fClickButton = BUTTON_MIDDLE;
			else if (fingers == 2)
				fClickButton = BUTTON_RIGHT;
			else if (fingers == 1 && presser != NULL
				&& presser->y < (int32)fHeight / 5
				&& presser->x > (int32)fWidth / 2)
				fClickButton = BUTTON_RIGHT;
			else
				fClickButton = BUTTON_LEFT;
		}
		movement->buttons = (movement->buttons & ~BUTTON_LEFT) | fClickButton;
	} else
		fClickButton = 0;

	// After a click, the rest of the touch is one finger to movement_maker,
	// even with more on the pad: it would scroll instead of drag, and turn
	// a held left button into the right one when a second finger comes.
	if (fContactClicked && count >= 2) {
		movement->fingers = 1;
		movement->fingerWidth = 5;
	}

	// Tapping with two fingers is a right click, with three a middle click.
	// It is reported as soon as the fingers start to lift; the rest of the
	// touch is dropped so movement_maker does not see a one finger tap.
	if (fContactMaxFingers >= 2 && count < 2 && !fContactMoved
		&& !fContactClicked && now - fContactStart < kMultiFingerTapTimeout) {
		uint32 button = fContactMaxFingers == 2 ? BUTTON_RIGHT : BUTTON_MIDDLE;
		_ResetContact();
		fLastCount = count;
		memset(movement, 0, sizeof(*movement));
		movement->buttons = button;
		movement->fingerWidth = 4;
		fIgnoreContact = count > 0;
		fPendingRelease = true;
	}

	return true;
}


void
AppleSPITouchpad::_ResetContact()
{
	fClickButton = 0;
	fLastCount = 0;
	fContactMaxFingers = 0;
	fContactStart = 0;
	fTapX = fTapY = 0;
	fContactMoved = false;
	fContactClicked = false;
	fIgnoreContact = false;
	fPendingRelease = false;
}


//	#pragma mark - event queue


void
AppleSPITouchpad::_Queue(const touchpad_movement& movement)
{
	MutexLocker locker(fQueueLock);
	if (fQueueCount == APPLE_SPI_TP_QUEUE) {
		// The reader fell behind; drop the oldest position, keeping the
		// semaphore count equal to the queue length.
		fQueueHead = (fQueueHead + 1) % APPLE_SPI_TP_QUEUE;
		fQueueCount--;
	} else
		release_sem_etc(fQueueSem, 1, B_DO_NOT_RESCHEDULE);

	fQueue[(fQueueHead + fQueueCount) % APPLE_SPI_TP_QUEUE] = movement;
	fQueueCount++;
}


status_t
AppleSPITouchpad::_Dequeue(touchpad_movement* movement, bigtime_t timeout)
{
	while (true) {
		if (fRemoved)
			return B_DEV_NOT_READY;

		status_t status = timeout < 0
			? acquire_sem_etc(fQueueSem, 1, B_CAN_INTERRUPT, 0)
			: acquire_sem_etc(fQueueSem, 1,
				B_RELATIVE_TIMEOUT | B_CAN_INTERRUPT, timeout);
		if (status != B_OK)
			return status == B_WOULD_BLOCK ? B_TIMED_OUT : status;

		MutexLocker locker(fQueueLock);
		if (fQueueCount == 0) {
			// woken by Removed(), or by a Close() racing with us
			continue;
		}

		*movement = fQueue[fQueueHead];
		fQueueHead = (fQueueHead + 1) % APPLE_SPI_TP_QUEUE;
		fQueueCount--;
		return B_OK;
	}
}
