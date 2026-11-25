/*
 * Copyright 2026, RenkuOS Contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */
#ifndef FACETIMEHD_PRODUCER_H
#define FACETIMEHD_PRODUCER_H


#include <BufferProducer.h>
#include <Controllable.h>
#include <Locker.h>
#include <MediaEventLooper.h>


extern const char* kFaceTimeHDDevice;


class FaceTimeHDProducer : public BMediaEventLooper, public BBufferProducer,
	public BControllable {
public:
								FaceTimeHDProducer(BMediaAddOn* addOn,
									const char* name, int32 internalID);
	virtual						~FaceTimeHDProducer();

	// BMediaNode
	virtual	BMediaAddOn*		AddOn(int32* _internalID) const;
	virtual	status_t			HandleMessage(int32 message, const void* data,
									size_t size);

protected:
	// BMediaEventLooper
	virtual	void				NodeRegistered();
	virtual	void				HandleEvent(const media_timed_event* event,
									bigtime_t lateness,
									bool realTimeEvent = false);

	// BBufferProducer
	virtual	status_t			FormatSuggestionRequested(media_type type,
									int32 quality, media_format* format);
	virtual	status_t			FormatProposal(const media_source& output,
									media_format* format);
	virtual	status_t			FormatChangeRequested(
									const media_source& source,
									const media_destination& destination,
									media_format* ioFormat,
									int32* _deprecated_);
	virtual	status_t			GetNextOutput(int32* cookie,
									media_output* _output);
	virtual	status_t			DisposeOutputCookie(int32 cookie);
	virtual	status_t			SetBufferGroup(const media_source& source,
									BBufferGroup* group);
	virtual	status_t			GetLatency(bigtime_t* _latency);
	virtual	status_t			PrepareToConnect(const media_source& what,
									const media_destination& where,
									media_format* format,
									media_source* _source, char* _name);
	virtual	void				Connect(status_t error,
									const media_source& source,
									const media_destination& destination,
									const media_format& format,
									char* ioName);
	virtual	void				Disconnect(const media_source& what,
									const media_destination& where);
	virtual	void				LateNoticeReceived(const media_source& what,
									bigtime_t howMuch,
									bigtime_t performanceTime);
	virtual	void				EnableOutput(const media_source& what,
									bool enabled, int32* _deprecated_);

	// BControllable
	virtual	status_t			GetParameterValue(int32 id,
									bigtime_t* _lastChange, void* value,
									size_t* _size);
	virtual	void				SetParameterValue(int32 id, bigtime_t when,
									const void* value, size_t size);

private:
			status_t			_Control(uint32 op, int32 id, int32* value);
			status_t			_SpecializeFormat(media_format* format) const;
			void				_StartCapture();
			void				_StopCapture();
	static	status_t			_CaptureThread(void* data);
			void				_Capture();
			void				_Convert(const uint8* frame, uint8* target,
									const media_raw_video_format& format);
			void				_SetUpScaling(
									const media_raw_video_format& format);

			BMediaAddOn*		fAddOn;
			int32				fInternalID;

			BLocker				fLock;
				// guards everything below that the capture thread uses
			media_output		fOutput;
			bool				fConnected;
			bool				fEnabled;
			BBufferGroup*		fBufferGroup;
			bool				fOwnsBufferGroup;
			uint32*				fColumnMap;
			uint32*				fRowMap;

			thread_id			fThread;
			volatile bool		fRunning;
			int					fDevice;
			uint8*				fFrame;
			uint32				fFrameSize;
			uint32				fCameraWidth;
			uint32				fCameraHeight;
			uint32				fFramesSent;
			bigtime_t			fLastControlChange;
};


#endif	// FACETIMEHD_PRODUCER_H
