/*
 * Copyright 2026, RenkuOS Contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */
#ifndef CAMERA_WINDOW_H
#define CAMERA_WINDOW_H


#include <Window.h>

#include "facetimehd_ioctl.h"


class BButton;
class BCheckBox;
class BMessageRunner;
class BStringView;
class LevelSlider;


class CameraWindow : public BWindow {
public:
								CameraWindow();
	virtual						~CameraWindow();

	virtual	void				MessageReceived(BMessage* message);
	virtual	bool				QuitRequested();

private:
			status_t			_Control(uint32 op, uint32 id, int32* value);
			void				_Load();
			void				_SetControl(uint32 id, int32 value);
			void				_UpdateStatus();
			void				_UpdateDefaultsButton();

			int					fDevice;
			LevelSlider*		fLevels[4];
			BCheckBox*			fWhiteBalance;
			BCheckBox*			fLowLight;
			BButton*			fDefaultsButton;
			BStringView*		fStatus;
			BMessageRunner*		fStatusRunner;
};


#endif	// CAMERA_WINDOW_H
