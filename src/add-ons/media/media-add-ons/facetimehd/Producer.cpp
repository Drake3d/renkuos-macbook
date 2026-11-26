/*
 * Copyright 2026, RenkuOS Contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


/*!	The FaceTime HD camera as a media node: a live video input that streams
	from the facetimehd driver while the node runs.

	The camera scales in hardware: the node asks it for the connection's
	size (rounded up to even), so it passes YUYV on unchanged as B_YCbCr422
	at any even size up to 1280x720, or converts it pixel for pixel to
	B_RGB32, which most video consumers (CodyCam among them) want. If the
	driver cannot choose the size, the camera runs at 1280x720 and the node
	cuts the largest centred window of the requested shape and scales it.
	The driver mirrors the picture if the Camera preferences say so.

	The camera is only turned on while the node is started.
*/


#include "Producer.h"

#include <errno.h>
#include <fcntl.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <Autolock.h>
#include <Buffer.h>
#include <BufferGroup.h>
#include <ParameterWeb.h>
#include <TimeSource.h>

#include "facetimehd_ioctl.h"
#include "facetimehd_settings.h"


const char* kFaceTimeHDDevice = "/dev/video/facetimehd/0";

static const float kFrameRate = 30.0f;
static const int32 kBufferCount = 4;
// from the frame's arrival in the driver to the buffer being sent
static const bigtime_t kProcessingLatency = 15000;

// picture controls as shown: -kLevelRange to +kLevelRange
static const float kLevelRange = 100.0f;
// ids of the group titles, apart from the driver's FTHD_CONTROL_*
static const int32 kPictureTitle = 100;
static const int32 kLightTitle = 101;
// Picture size: one menu for the two size controls; its value is an index
// into kFTHDPictureSizes.
static const int32 kPictureSize = 102;


// into the system log, next to the driver's own messages
#define TRACE(x...)		syslog(LOG_INFO, "facetimehd media add-on: " x)


FaceTimeHDProducer::FaceTimeHDProducer(BMediaAddOn* addOn, const char* name,
	int32 internalID)
	:
	BMediaNode(name),
	BMediaEventLooper(),
	BBufferProducer(B_MEDIA_RAW_VIDEO),
	BControllable(),
	fAddOn(addOn),
	fInternalID(internalID),
	fLock("facetimehd producer"),
	fConnected(false),
	fEnabled(false),
	fBufferGroup(NULL),
	fOwnsBufferGroup(false),
	fColumnMap(NULL),
	fRowMap(NULL),
	fThread(-1),
	fRunning(false),
	fDevice(-1),
	fFrame(NULL),
	fFrameSize(0),
	fCameraWidth(FTHD_MAX_WIDTH),
	fCameraHeight(FTHD_MAX_HEIGHT),
	fFrameWidth(FTHD_DEFAULT_WIDTH),
	fFrameHeight(FTHD_DEFAULT_HEIGHT),
	fFramesSent(0),
	fLastControlChange(0)
{
	fOutput.destination = media_destination::null;
	// what makes the media server offer it as the default video input
	AddNodeKind(B_PHYSICAL_INPUT);
}


FaceTimeHDProducer::~FaceTimeHDProducer()
{
	// stop the control loop first, so no event handler runs meanwhile
	Quit();
	_StopCapture();
	if (fConnected)
		Disconnect(fOutput.source, fOutput.destination);
}


BMediaAddOn*
FaceTimeHDProducer::AddOn(int32* _internalID) const
{
	if (_internalID != NULL)
		*_internalID = fInternalID;
	return fAddOn;
}


status_t
FaceTimeHDProducer::HandleMessage(int32 message, const void* data,
	size_t size)
{
	return B_ERROR;
}


// #pragma mark - BMediaEventLooper


void
FaceTimeHDProducer::NodeRegistered()
{
	fOutput.node = Node();
	fOutput.source.port = ControlPort();
	fOutput.source.id = 0;
	fOutput.destination = media_destination::null;
	strlcpy(fOutput.name, "Video", sizeof(fOutput.name));

	fOutput.format.type = B_MEDIA_RAW_VIDEO;
	fOutput.format.u.raw_video = media_raw_video_format::wildcard;

	// Picture controls; their ids are the driver's FTHD_CONTROL_*. The
	// default theme draws a B_GENERIC continuous parameter as a horizontal
	// slider (B_GAIN would make it a vertical mixer fader), a B_ENABLE
	// discrete one as a check box, and a null parameter that comes first in
	// its group as the group's title.
	BParameterWeb* web = new BParameterWeb();
	BParameterGroup* main = web->MakeGroup(Name());

	BParameterGroup* picture = main->MakeGroup("Picture");
	picture->MakeNullParameter(kPictureTitle, B_MEDIA_RAW_VIDEO, "Picture",
		B_GENERIC);
	const struct {
		int32		id;
		const char*	name;
	} kLevels[] = {
		{ FTHD_CONTROL_BRIGHTNESS, "Brightness" },
		{ FTHD_CONTROL_CONTRAST, "Contrast" },
		{ FTHD_CONTROL_SATURATION, "Saturation" },
		{ FTHD_CONTROL_HUE, "Hue" }
	};
	for (size_t i = 0; i < B_COUNT_OF(kLevels); i++) {
		picture->MakeContinuousParameter(kLevels[i].id, B_MEDIA_RAW_VIDEO,
			kLevels[i].name, B_GENERIC, "", -kLevelRange, kLevelRange, 1);
	}
	// The same settings, in the same order, as the Camera preferences.
	picture->MakeDiscreteParameter(FTHD_CONTROL_MIRROR, B_MEDIA_RAW_VIDEO,
		"Mirror picture (left and right swapped, like a mirror)", B_ENABLE);
	BDiscreteParameter* size = picture->MakeDiscreteParameter(kPictureSize,
		B_MEDIA_RAW_VIDEO, "Picture size (for apps that let the camera "
		"choose)", B_GENERIC);
	for (int32 i = 0; i < kFTHDPictureSizeCount; i++)
		size->AddItem(i, kFTHDPictureSizes[i].label);

	BParameterGroup* light = main->MakeGroup("Colour and light");
	light->MakeNullParameter(kLightTitle, B_MEDIA_RAW_VIDEO,
		"Colour and light", B_GENERIC);
	light->MakeDiscreteParameter(FTHD_CONTROL_AUTO_WHITE_BALANCE,
		B_MEDIA_RAW_VIDEO, "Automatic white balance", B_ENABLE);
	light->MakeDiscreteParameter(FTHD_CONTROL_LOW_LIGHT, B_MEDIA_RAW_VIDEO,
		"Brighter picture in low light (5 frames per second)", B_ENABLE);
	SetParameterWeb(web);

	SetPriority(B_URGENT_DISPLAY_PRIORITY);
	Run();
	TRACE("node %" B_PRId32 " registered\n", ID());
}


void
FaceTimeHDProducer::HandleEvent(const media_timed_event* event,
	bigtime_t lateness, bool realTimeEvent)
{
	switch (event->type) {
		case BTimedEventQueue::B_START:
		{
			// the camera only runs while someone also takes its frames
			BAutolock locker(fLock);
			bool connected = fConnected;
			locker.Unlock();
			if (connected)
				_StartCapture();
			break;
		}
		case BTimedEventQueue::B_STOP:
			_StopCapture();
			// drop the frames already scheduled
			EventQueue()->FlushEvents(0, BTimedEventQueue::B_ALWAYS, true,
				BTimedEventQueue::B_HANDLE_BUFFER);
			break;
		default:
			break;
	}
}


// #pragma mark - formats


/*!	Fills in the wildcards of \a format and checks that the camera can
	deliver it.
*/
/*!	The picture size chosen in the Camera preferences: what the driver
	gives a newly opened device. Opening it does not switch the camera on.
*/
static void
default_size(uint32* _width, uint32* _height)
{
	*_width = FTHD_DEFAULT_WIDTH;
	*_height = FTHD_DEFAULT_HEIGHT;

	int device = open(kFaceTimeHDDevice, O_RDWR);
	if (device < 0)
		return;
	fthd_info info;
	if (ioctl(device, FTHD_GET_INFO, &info, sizeof(info)) == 0
		&& info.version == FTHD_IOCTL_VERSION && info.width != 0
		&& info.height != 0) {
		*_width = info.width;
		*_height = info.height;
	}
	close(device);
}


status_t
FaceTimeHDProducer::_SpecializeFormat(media_format* format) const
{
	if (format->type == B_MEDIA_UNKNOWN_TYPE)
		format->type = B_MEDIA_RAW_VIDEO;
	if (format->type != B_MEDIA_RAW_VIDEO)
		return B_MEDIA_BAD_FORMAT;

	media_raw_video_format& video = format->u.raw_video;
	const media_raw_video_format& wildcard = media_raw_video_format::wildcard;

	if (video.display.format == wildcard.display.format)
		video.display.format = B_RGB32;
	if (video.display.format != B_RGB32
		&& video.display.format != B_YCbCr422) {
		return B_MEDIA_BAD_FORMAT;
	}

	// Sizes the consumer leaves open: the Camera preferences' choice.
	if (video.display.line_width == wildcard.display.line_width
		|| video.display.line_count == wildcard.display.line_count) {
		uint32 defaultWidth;
		uint32 defaultHeight;
		default_size(&defaultWidth, &defaultHeight);
		if (video.display.line_width == wildcard.display.line_width)
			video.display.line_width = defaultWidth;
		if (video.display.line_count == wildcard.display.line_count)
			video.display.line_count = defaultHeight;
	}
	uint32 width = video.display.line_width;
	uint32 height = video.display.line_count;
	if (width < 2 || height < 2 || width > fCameraWidth
		|| height > fCameraHeight) {
		return B_MEDIA_BAD_FORMAT;
	}
	// YCbCr goes out as the camera made it: two pixels per four bytes, and
	// the camera only makes even sizes
	if (video.display.format == B_YCbCr422
		&& ((width & 1) != 0 || (height & 1) != 0)) {
		return B_MEDIA_BAD_FORMAT;
	}

	uint32 bytesPerRow = width * (video.display.format == B_RGB32 ? 4 : 2);
	if (video.display.bytes_per_row == wildcard.display.bytes_per_row)
		video.display.bytes_per_row = bytesPerRow;
	if (video.display.bytes_per_row != bytesPerRow)
		return B_MEDIA_BAD_FORMAT;

	video.display.pixel_offset = 0;
	video.display.line_offset = 0;
	video.display.flags = 0;
	video.field_rate = kFrameRate;
	video.interlace = 1;
	video.first_active = 0;
	video.last_active = height - 1;
	video.orientation = B_VIDEO_TOP_LEFT_RIGHT;
	video.pixel_width_aspect = 1;
	video.pixel_height_aspect = 1;
	return B_OK;
}


status_t
FaceTimeHDProducer::FormatSuggestionRequested(media_type type, int32 quality,
	media_format* format)
{
	if (type != B_MEDIA_RAW_VIDEO && type != B_MEDIA_UNKNOWN_TYPE)
		return B_MEDIA_BAD_FORMAT;

	format->Clear();
	format->type = B_MEDIA_RAW_VIDEO;
	format->u.raw_video = media_raw_video_format::wildcard;
	return _SpecializeFormat(format);
}


status_t
FaceTimeHDProducer::FormatProposal(const media_source& output,
	media_format* format)
{
	if (output != fOutput.source)
		return B_MEDIA_BAD_SOURCE;

	media_format proposal = *format;
	status_t status = _SpecializeFormat(&proposal);
	if (status != B_OK) {
		// offer what we would do instead
		format->Clear();
		format->type = B_MEDIA_RAW_VIDEO;
		format->u.raw_video = media_raw_video_format::wildcard;
		_SpecializeFormat(format);
		return status;
	}
	*format = proposal;
	return B_OK;
}


status_t
FaceTimeHDProducer::FormatChangeRequested(const media_source& source,
	const media_destination& destination, media_format* ioFormat,
	int32* _deprecated_)
{
	return B_ERROR;
}


status_t
FaceTimeHDProducer::GetNextOutput(int32* cookie, media_output* _output)
{
	if (*cookie != 0)
		return B_BAD_INDEX;

	BAutolock locker(fLock);
	*_output = fOutput;
	(*cookie)++;
	return B_OK;
}


status_t
FaceTimeHDProducer::DisposeOutputCookie(int32 cookie)
{
	return B_OK;
}


// #pragma mark - connections


status_t
FaceTimeHDProducer::SetBufferGroup(const media_source& source,
	BBufferGroup* group)
{
	if (source != fOutput.source)
		return B_MEDIA_BAD_SOURCE;

	BAutolock locker(fLock);
	if (fOwnsBufferGroup)
		delete fBufferGroup;

	if (group != NULL) {
		// the consumer's own buffers, as CodyCam provides them
		fBufferGroup = group;
		fOwnsBufferGroup = false;
		return B_OK;
	}

	fBufferGroup = new(std::nothrow) BBufferGroup(
		fOutput.format.u.raw_video.display.bytes_per_row
			* fOutput.format.u.raw_video.display.line_count, kBufferCount);
	fOwnsBufferGroup = true;
	if (fBufferGroup == NULL || fBufferGroup->InitCheck() != B_OK) {
		delete fBufferGroup;
		fBufferGroup = NULL;
		return B_NO_MEMORY;
	}
	return B_OK;
}


status_t
FaceTimeHDProducer::GetLatency(bigtime_t* _latency)
{
	*_latency = EventLatency() + SchedulingLatency();
	return B_OK;
}


status_t
FaceTimeHDProducer::PrepareToConnect(const media_source& what,
	const media_destination& where, media_format* format,
	media_source* _source, char* _name)
{
	if (what != fOutput.source)
		return B_MEDIA_BAD_SOURCE;

	BAutolock locker(fLock);
	if (fConnected || fOutput.destination != media_destination::null)
		return B_MEDIA_ALREADY_CONNECTED;

	status_t status = _SpecializeFormat(format);
	if (status != B_OK)
		return status;

	fOutput.destination = where;
	*_source = fOutput.source;
	strlcpy(_name, fOutput.name, B_MEDIA_NAME_LENGTH);
	return B_OK;
}


void
FaceTimeHDProducer::Connect(status_t error, const media_source& source,
	const media_destination& destination, const media_format& format,
	char* ioName)
{
	BAutolock locker(fLock);
	if (error != B_OK || source != fOutput.source) {
		fOutput.destination = media_destination::null;
		return;
	}

	fOutput.destination = destination;
	fOutput.format = format;
	strlcpy(ioName, fOutput.name, B_MEDIA_NAME_LENGTH);

	const media_raw_video_format& video = format.u.raw_video;
	fBufferGroup = new(std::nothrow) BBufferGroup(
		video.display.bytes_per_row * video.display.line_count,
		kBufferCount);
	fOwnsBufferGroup = true;
	if (fBufferGroup == NULL || fBufferGroup->InitCheck() != B_OK) {
		TRACE("creating the buffer group failed\n");
		delete fBufferGroup;
		fBufferGroup = NULL;
		fOutput.destination = media_destination::null;
		return;
	}
	_ChooseFrameSize(video);
	_SetUpScaling(video);

	bigtime_t latency = 0;
	media_node_id timeSource;
	FindLatencyFor(fOutput.destination, &latency, &timeSource);
	SetEventLatency(latency + kProcessingLatency);

	fConnected = true;
	fEnabled = true;
	TRACE("connected: %" B_PRIu32 "x%" B_PRIu32 " %s\n",
		video.display.line_width, video.display.line_count,
		video.display.format == B_RGB32 ? "RGB32" : "YCbCr422");
	locker.Unlock();

	// connected to a node that was started already
	if (RunState() == B_STARTED)
		_StartCapture();
}


void
FaceTimeHDProducer::Disconnect(const media_source& what,
	const media_destination& where)
{
	{
		BAutolock locker(fLock);
		if (what != fOutput.source || where != fOutput.destination)
			return;
		fConnected = false;
	}

	// Some apps (CodyCam) only disconnect and never stop the node: turn
	// the camera off now. Not under fLock, which the capture thread takes.
	_StopCapture();

	BAutolock locker(fLock);
	fEnabled = false;
	fOutput.destination = media_destination::null;
	fOutput.format.u.raw_video = media_raw_video_format::wildcard;
	if (fOwnsBufferGroup)
		delete fBufferGroup;
	fBufferGroup = NULL;
	fOwnsBufferGroup = false;
	free(fColumnMap);
	free(fRowMap);
	fColumnMap = fRowMap = NULL;
}


void
FaceTimeHDProducer::LateNoticeReceived(const media_source& what,
	bigtime_t howMuch, bigtime_t performanceTime)
{
	// a live source cannot catch up; frames that are late are just late
}


void
FaceTimeHDProducer::EnableOutput(const media_source& what, bool enabled,
	int32* _deprecated_)
{
	if (what != fOutput.source)
		return;

	BAutolock locker(fLock);
	fEnabled = enabled;
}


// #pragma mark - picture controls


/*!	Gets or sets a control in the driver, which keeps the values for every
	stream. The device is opened just for this, which does not turn the
	camera on.
*/
status_t
FaceTimeHDProducer::_Control(uint32 op, int32 id, int32* value)
{
	int fd = open(kFaceTimeHDDevice, O_RDWR);
	if (fd < 0)
		return errno;

	fthd_control control = { (uint32)id, *value };
	status_t status = ioctl(fd, op, &control, sizeof(control)) == 0
		? B_OK : errno;
	close(fd);
	*value = control.value;
	return status;
}


static bool
is_level(int32 id)
{
	return id == FTHD_CONTROL_BRIGHTNESS || id == FTHD_CONTROL_CONTRAST
		|| id == FTHD_CONTROL_SATURATION || id == FTHD_CONTROL_HUE;
}


/*!	The sliders show -100 to +100 with 0 as the camera's own setting; the
	driver takes 0 to 255 with 128 there.
*/
static float
level_for_driver(int32 value)
{
	return (value - 128) * kLevelRange / 127.0f;
}


static int32
driver_for_level(float level)
{
	if (level < -kLevelRange)
		level = -kLevelRange;
	if (level > kLevelRange)
		level = kLevelRange;
	float value = 128 + level * 127.0f / kLevelRange;
	return (int32)(value + (value >= 0 ? 0.5f : -0.5f));
}


/*!	The index into kFTHDPictureSizes of the camera's picture size setting,
	or the first one if the setting is not in the list.
*/
static int32
size_index(int32 width, int32 height)
{
	for (int32 i = 0; i < kFTHDPictureSizeCount; i++) {
		if (kFTHDPictureSizes[i].width == width
			&& kFTHDPictureSizes[i].height == height)
			return i;
	}
	return 0;
}


/*!	Keeps the settings for the next boot, in the same file the Camera
	preferences write.
*/
static void
save_settings()
{
	int fd = open(kFaceTimeHDDevice, O_RDWR);
	if (fd < 0)
		return;
	status_t status = fthd_save_settings(fd);
	close(fd);
	if (status != B_OK)
		TRACE("saving the settings failed: %s\n", strerror(status));
}


status_t
FaceTimeHDProducer::GetParameterValue(int32 id, bigtime_t* _lastChange,
	void* value, size_t* _size)
{
	int32 current = 0;
	if (id == kPictureSize) {
		int32 width = 0;
		int32 height = 0;
		status_t status = _Control(FTHD_GET_CONTROL, FTHD_CONTROL_WIDTH,
			&width);
		if (status == B_OK)
			status = _Control(FTHD_GET_CONTROL, FTHD_CONTROL_HEIGHT, &height);
		if (status != B_OK)
			return status;
		current = size_index(width, height);
	} else if (id >= 0 && id < FTHD_CONTROL_COUNT && id != FTHD_CONTROL_WIDTH
		&& id != FTHD_CONTROL_HEIGHT) {
		status_t status = _Control(FTHD_GET_CONTROL, id, &current);
		if (status != B_OK)
			return status;
	} else
		return B_BAD_VALUE;

	*_lastChange = fLastControlChange;
	if (is_level(id)) {
		if (*_size < sizeof(float))
			return B_NO_MEMORY;
		*(float*)value = level_for_driver(current);
		*_size = sizeof(float);
	} else {
		if (*_size < sizeof(int32))
			return B_NO_MEMORY;
		*(int32*)value = current;
		*_size = sizeof(int32);
	}
	return B_OK;
}


void
FaceTimeHDProducer::SetParameterValue(int32 id, bigtime_t when,
	const void* value, size_t size)
{
	if (value == NULL)
		return;

	int32 newValue;
	if (id == kPictureSize) {
		if (size < sizeof(int32))
			return;
		newValue = *(const int32*)value;
		if (newValue < 0 || newValue >= kFTHDPictureSizeCount)
			return;
		// Used by streams started from now on.
		int32 width = kFTHDPictureSizes[newValue].width;
		int32 height = kFTHDPictureSizes[newValue].height;
		if (_Control(FTHD_SET_CONTROL, FTHD_CONTROL_WIDTH, &width) != B_OK
			|| _Control(FTHD_SET_CONTROL, FTHD_CONTROL_HEIGHT, &height)
				!= B_OK) {
			TRACE("setting the picture size failed\n");
			return;
		}
	} else if (id >= 0 && id < FTHD_CONTROL_COUNT && id != FTHD_CONTROL_WIDTH
		&& id != FTHD_CONTROL_HEIGHT) {
		if (is_level(id)) {
			if (size < sizeof(float))
				return;
			newValue = driver_for_level(*(const float*)value);
		} else {
			if (size < sizeof(int32))
				return;
			newValue = *(const int32*)value != 0 ? 1 : 0;
		}

		if (_Control(FTHD_SET_CONTROL, id, &newValue) != B_OK) {
			TRACE("setting control %" B_PRId32 " failed\n", id);
			return;
		}
	} else
		return;

	save_settings();

	fLastControlChange = when;
	if (is_level(id)) {
		float level = level_for_driver(newValue);
		BroadcastNewParameterValue(when, id, &level, sizeof(level));
	} else
		BroadcastNewParameterValue(when, id, &newValue, sizeof(newValue));
}


// #pragma mark - capture


void
FaceTimeHDProducer::_StartCapture()
{
	if (fRunning)
		return;

	fDevice = open(kFaceTimeHDDevice, O_RDWR);
	if (fDevice < 0) {
		TRACE("opening %s failed: %s\n", kFaceTimeHDDevice, strerror(errno));
		ReportError(B_NODE_IN_DISTRESS);
		return;
	}

	// Ask the camera for the connection's size; scale in software from
	// 1280x720 if it cannot.
	{
		BAutolock locker(fLock);
		fthd_size size = { fFrameWidth, fFrameHeight };
		if (ioctl(fDevice, FTHD_SET_SIZE, &size, sizeof(size)) != 0) {
			TRACE("the camera cannot make %" B_PRIu32 "x%" B_PRIu32 " (%s); "
				"scaling 1280x720\n", fFrameWidth, fFrameHeight,
				strerror(errno));
			fFrameWidth = FTHD_DEFAULT_WIDTH;
			fFrameHeight = FTHD_DEFAULT_HEIGHT;
			_SetUpScaling(fOutput.format.u.raw_video);
		}
	}

	fthd_info info;
	if (ioctl(fDevice, FTHD_GET_INFO, &info, sizeof(info)) != 0
		|| info.version != FTHD_IOCTL_VERSION || info.width != fFrameWidth
		|| info.height != fFrameHeight) {
		TRACE("the driver does not match this add-on\n");
		close(fDevice);
		fDevice = -1;
		ReportError(B_NODE_IN_DISTRESS);
		return;
	}
	TRACE("camera frames %" B_PRIu32 "x%" B_PRIu32 "\n", fFrameWidth,
		fFrameHeight);

	fFrameSize = info.frame_size;
	fFrame = (uint8*)malloc(fFrameSize);
	if (fFrame == NULL || ioctl(fDevice, FTHD_START, NULL, 0) != 0) {
		TRACE("starting the camera failed: %s\n", strerror(errno));
		free(fFrame);
		fFrame = NULL;
		close(fDevice);
		fDevice = -1;
		ReportError(B_NODE_IN_DISTRESS);
		return;
	}

	fFramesSent = 0;
	fRunning = true;
	fThread = spawn_thread(_CaptureThread, "facetimehd capture",
		B_URGENT_DISPLAY_PRIORITY, this);
	if (fThread < 0 || resume_thread(fThread) != B_OK) {
		fRunning = false;
		_StopCapture();
		return;
	}
	TRACE("camera started\n");
}


void
FaceTimeHDProducer::_StopCapture()
{
	fRunning = false;
	if (fThread >= 0) {
		status_t result;
		wait_for_thread(fThread, &result);
		fThread = -1;
	}

	if (fDevice >= 0) {
		ioctl(fDevice, FTHD_STOP, NULL, 0);
		close(fDevice);
		fDevice = -1;
		TRACE("camera stopped after %" B_PRIu32 " frames\n", fFramesSent);
	}
	free(fFrame);
	fFrame = NULL;
}


status_t
FaceTimeHDProducer::_CaptureThread(void* data)
{
	((FaceTimeHDProducer*)data)->_Capture();
	return B_OK;
}


void
FaceTimeHDProducer::_Capture()
{
	bool reportedTimeout = false;

	while (fRunning) {
		fthd_frame_request request = {};
		request.buffer = fFrame;
		request.buffer_size = fFrameSize;
		// short, so that stopping does not wait long
		request.timeout = 250000;
		if (ioctl(fDevice, FTHD_READ_FRAME, &request, sizeof(request)) != 0) {
			if (errno == B_TIMED_OUT || errno == B_INTERRUPTED) {
				if (!reportedTimeout && fRunning) {
					TRACE("no frame from the camera within 250 ms\n");
					reportedTimeout = true;
				}
				continue;
			}
			TRACE("reading a frame failed: %s\n", strerror(errno));
			snooze(100000);
			continue;
		}

		BAutolock locker(fLock);
		if (!fConnected || !fEnabled || fBufferGroup == NULL)
			continue;

		const media_raw_video_format& video = fOutput.format.u.raw_video;
		size_t size = video.display.bytes_per_row * video.display.line_count;
		BBuffer* buffer = fBufferGroup->RequestBuffer(size, 10000);
		if (buffer == NULL)
			continue;
			// the consumer still holds every buffer: drop this frame

		_Convert(fFrame, (uint8*)buffer->Data(), video);

		media_header* header = buffer->Header();
		header->type = B_MEDIA_RAW_VIDEO;
		header->size_used = size;
		header->time_source = TimeSource()->ID();
		header->start_time = TimeSource()->PerformanceTimeFor(system_time());
		header->u.raw_video.field_sequence = request.sequence;
		header->u.raw_video.field_number = 0;
		header->u.raw_video.pulldown_number = 0;
		header->u.raw_video.first_active_line = 0;
		header->u.raw_video.line_count = video.display.line_count;

		if (SendBuffer(buffer, fOutput.source, fOutput.destination) != B_OK)
			buffer->Recycle();
		else
			fFramesSent++;
	}
}


/*!	The size to ask the camera for: the output's, rounded up to even and
	to the camera's smallest size. Software scaling covers the difference.
*/
void
FaceTimeHDProducer::_ChooseFrameSize(const media_raw_video_format& format)
{
	uint32 width = (format.display.line_width + 1) & ~1u;
	uint32 height = (format.display.line_count + 1) & ~1u;
	fFrameWidth = width < FTHD_MIN_WIDTH ? FTHD_MIN_WIDTH
		: width > fCameraWidth ? fCameraWidth : width;
	fFrameHeight = height < FTHD_MIN_HEIGHT ? FTHD_MIN_HEIGHT
		: height > fCameraHeight ? fCameraHeight : height;
}


/*!	Picks, for each output column and row, the camera pixel it shows: the
	largest centred window of the output's shape in the camera frame,
	scaled to fit (one to one when the camera made the output's size).
*/
void
FaceTimeHDProducer::_SetUpScaling(const media_raw_video_format& format)
{
	free(fColumnMap);
	free(fRowMap);
	fColumnMap = fRowMap = NULL;
	if (format.display.format != B_RGB32)
		return;

	uint32 width = format.display.line_width;
	uint32 height = format.display.line_count;
	uint32 windowWidth = fFrameWidth;
	uint32 windowHeight = (uint64)fFrameWidth * height / width;
	if (windowHeight > fFrameHeight) {
		windowHeight = fFrameHeight;
		windowWidth = (uint64)fFrameHeight * width / height;
	}
	uint32 left = (fFrameWidth - windowWidth) / 2;
	uint32 top = (fFrameHeight - windowHeight) / 2;

	fColumnMap = (uint32*)malloc(width * sizeof(uint32));
	fRowMap = (uint32*)malloc(height * sizeof(uint32));
	if (fColumnMap == NULL || fRowMap == NULL) {
		free(fColumnMap);
		free(fRowMap);
		fColumnMap = fRowMap = NULL;
		return;
	}
	for (uint32 x = 0; x < width; x++)
		fColumnMap[x] = left + (uint64)x * windowWidth / width;
	for (uint32 y = 0; y < height; y++)
		fRowMap[y] = top + (uint64)y * windowHeight / height;
}


static inline uint8
clamp8(int32 value)
{
	return value < 0 ? 0 : value > 255 ? 255 : value;
}


void
FaceTimeHDProducer::_Convert(const uint8* frame, uint8* target,
	const media_raw_video_format& format)
{
	uint32 width = format.display.line_width;
	uint32 height = format.display.line_count;
	uint32 cameraRow = fFrameWidth * 2;

	if (format.display.format == B_YCbCr422) {
		// the camera made exactly this size
		if (fFrameWidth == width && fFrameHeight == height)
			memcpy(target, frame, cameraRow * fFrameHeight);
		return;
	}
	if (fColumnMap == NULL || fRowMap == NULL)
		return;

	// YUYV: Y0 U Y1 V for each pair of pixels; ITU-R BT.601, limited range
	uint32* pixel = (uint32*)target;
	for (uint32 y = 0; y < height; y++) {
		const uint8* row = frame + fRowMap[y] * cameraRow;
		for (uint32 x = 0; x < width; x++) {
			uint32 column = fColumnMap[x];
			const uint8* pair = row + (column & ~1u) * 2;
			int32 luma = (row[column * 2] - 16) * 298 + 128;
			int32 u = pair[1] - 128;
			int32 v = pair[3] - 128;
			uint8 red = clamp8((luma + 409 * v) >> 8);
			uint8 green = clamp8((luma - 100 * u - 208 * v) >> 8);
			uint8 blue = clamp8((luma + 516 * u) >> 8);
			*pixel++ = 0xff000000 | (red << 16) | (green << 8) | blue;
		}
	}
}
