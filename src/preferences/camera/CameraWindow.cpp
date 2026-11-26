/*
 * Copyright 2026, RenkuOS Contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


/*!	Picture settings of the FaceTime HD camera. They go straight to the
	facetimehd driver, which applies them at once if the camera is on and
	keeps them for every app that uses it. They are also saved to the
	driver's settings file, which the driver reads when it loads, so they
	survive a restart.
*/


#include "CameraWindow.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <Application.h>
#include <File.h>
#include <FindDirectory.h>
#include <Path.h>
#include <Box.h>
#include <Button.h>
#include <CheckBox.h>
#include <ControlLook.h>
#include <Directory.h>
#include <Font.h>
#include <LayoutBuilder.h>
#include <MessageRunner.h>
#include <Slider.h>
#include <SpaceLayoutItem.h>
#include <String.h>
#include <StringView.h>


static const char* kDevice = "/dev/video/facetimehd/0";

// shown as -kRange to +kRange, with 0 as the camera's own setting
static const int32 kRange = 100;
// the driver's 0-255 value for 0
static const int32 kDriverMiddle = 128;

enum {
	kMsgLevelChanged		= 'lvch',
	kMsgWhiteBalance		= 'wbal',
	kMsgLowLight			= 'lowl',
	kMsgDefaults			= 'dflt',
	kMsgUpdateStatus		= 'stat',
	kMsgSave				= 'save'
};

// Wait this long after the last change before writing the settings file,
// so dragging a slider writes it once.
static const bigtime_t kSaveDelay = 500000;


static int32
level_for_driver(int32 value)
{
	int32 level = ((value - kDriverMiddle) * kRange * 2 + 127) / 254;
	return level < -kRange ? -kRange : level > kRange ? kRange : level;
}


static int32
driver_for_level(int32 level)
{
	int32 value = kDriverMiddle + (level * 254 + (level >= 0 ? kRange : -kRange))
		/ (kRange * 2);
	return value < 0 ? 0 : value > 255 ? 255 : value;
}


// #pragma mark - LevelSlider


/*!	A wide slider that shows its name on the left and its value, signed, on
	the right, with marks at -100, -50, 0, +50 and +100.
*/
class LevelSlider : public BSlider {
public:
	LevelSlider(const char* name, const char* label, uint32 control)
		:
		BSlider(name, label, NULL, -kRange, kRange, B_HORIZONTAL,
			B_TRIANGLE_THUMB),
		fControl(control)
	{
		BMessage* message = new BMessage(kMsgLevelChanged);
		message->AddUInt32("control", control);
		SetMessage(message);
		SetModificationMessage(new BMessage(*message));

		SetHashMarks(B_HASH_MARKS_BOTTOM);
		SetHashMarkCount(5);
		SetLimitLabels("\xE2\x88\x92" "100", "+100");
		SetKeyIncrementValue(5);
		SetExplicitMinSize(BSize(be_control_look->DefaultLabelSpacing() * 60,
			B_SIZE_UNSET));
		SetValue(0);
	}

	virtual const char* UpdateText() const
	{
		int32 value = Value();
		if (value > 0)
			snprintf(fText, sizeof(fText), "+%" B_PRId32, value);
		else if (value < 0)
			snprintf(fText, sizeof(fText), "\xE2\x88\x92%" B_PRId32, -value);
		else
			strlcpy(fText, "0", sizeof(fText));
		return fText;
	}

	virtual float MaxUpdateTextWidth()
	{
		return StringWidth("\xE2\x88\x92" "100");
	}

	uint32 Control() const
	{
		return fControl;
	}

private:
	uint32			fControl;
	mutable char	fText[16];
};


// #pragma mark - CameraWindow


CameraWindow::CameraWindow()
	:
	BWindow(BRect(100, 100, 500, 400), "Camera", B_TITLED_WINDOW,
		B_NOT_ZOOMABLE | B_NOT_V_RESIZABLE | B_AUTO_UPDATE_SIZE_LIMITS
			| B_ASYNCHRONOUS_CONTROLS | B_QUIT_ON_WINDOW_CLOSE),
	fDevice(-1),
	fStatusRunner(NULL),
	fSaveRunner(NULL),
	fSavePending(false)
{
	// header
	BStringView* title = new BStringView("title", "FaceTime HD camera");
	BFont font(be_bold_font);
	font.SetSize(font.Size() * 1.3f);
	title->SetFont(&font);

	fStatus = new BStringView("status", "");
	fStatus->SetHighUIColor(B_PANEL_TEXT_COLOR, B_DARKEN_1_TINT);

	// picture
	const struct {
		const char*	name;
		uint32		control;
	} kLevels[] = {
		{ "Brightness", FTHD_CONTROL_BRIGHTNESS },
		{ "Contrast", FTHD_CONTROL_CONTRAST },
		{ "Saturation", FTHD_CONTROL_SATURATION },
		{ "Hue", FTHD_CONTROL_HUE }
	};
	BBox* pictureBox = new BBox("picture");
	pictureBox->SetLabel("Picture");
	BGroupLayout* pictureLayout = BLayoutBuilder::Group<>(B_VERTICAL,
		B_USE_DEFAULT_SPACING)
		.SetInsets(B_USE_DEFAULT_SPACING, B_USE_BIG_SPACING,
			B_USE_DEFAULT_SPACING, B_USE_DEFAULT_SPACING);
	for (int32 i = 0; i < 4; i++) {
		fLevels[i] = new LevelSlider(kLevels[i].name, kLevels[i].name,
			kLevels[i].control);
		pictureLayout->AddView(fLevels[i]);
	}
	pictureBox->AddChild(pictureLayout->View());

	// colour and light
	fWhiteBalance = new BCheckBox("white balance",
		"Automatic white balance", new BMessage(kMsgWhiteBalance));
	fLowLight = new BCheckBox("low light",
		"Brighter picture in low light", new BMessage(kMsgLowLight));
	BStringView* lowLightNote = new BStringView("low light note",
		"Lets the frame rate drop to 5 frames per second.");
	lowLightNote->SetHighUIColor(B_PANEL_TEXT_COLOR, B_DARKEN_1_TINT);
	BFont small(be_plain_font);
	small.SetSize(small.Size() * 0.9f);
	lowLightNote->SetFont(&small);

	BBox* lightBox = new BBox("light");
	lightBox->SetLabel("Colour and light");
	BLayoutBuilder::Group<>(lightBox, B_VERTICAL, B_USE_HALF_ITEM_SPACING)
		.SetInsets(B_USE_DEFAULT_SPACING, B_USE_BIG_SPACING,
			B_USE_DEFAULT_SPACING, B_USE_DEFAULT_SPACING)
		.Add(fWhiteBalance)
		.Add(fLowLight)
		.AddGroup(B_HORIZONTAL, 0)
			.Add(BSpaceLayoutItem::CreateHorizontalStrut(
				be_control_look->DefaultLabelSpacing() * 3))
			.Add(lowLightNote)
			.AddGlue()
		.End();

	fDefaultsButton = new BButton("defaults", "Defaults",
		new BMessage(kMsgDefaults));

	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
		.SetInsets(B_USE_WINDOW_SPACING)
		.AddGroup(B_VERTICAL, 0)
			.Add(title)
			.Add(fStatus)
		.End()
		.Add(pictureBox)
		.Add(lightBox)
		.AddGroup(B_HORIZONTAL)
			.Add(fDefaultsButton)
			.AddGlue()
		.End();

	_Load();
	CenterOnScreen();

	BMessage update(kMsgUpdateStatus);
	fStatusRunner = new BMessageRunner(BMessenger(this), &update, 1000000);
}


CameraWindow::~CameraWindow()
{
	delete fStatusRunner;
	delete fSaveRunner;
	if (fDevice >= 0)
		close(fDevice);
}


bool
CameraWindow::QuitRequested()
{
	if (fSavePending)
		_SaveSettings();
	return true;
}


void
CameraWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgLevelChanged:
		{
			uint32 control;
			if (message->FindUInt32("control", &control) != B_OK)
				break;
			for (int32 i = 0; i < 4; i++) {
				if (fLevels[i]->Control() == control) {
					_SetControl(control,
						driver_for_level(fLevels[i]->Value()));
				}
			}
			_UpdateDefaultsButton();
			_ScheduleSave();
			break;
		}

		case kMsgWhiteBalance:
			_SetControl(FTHD_CONTROL_AUTO_WHITE_BALANCE,
				fWhiteBalance->Value() == B_CONTROL_ON);
			_UpdateDefaultsButton();
			_ScheduleSave();
			break;

		case kMsgLowLight:
			_SetControl(FTHD_CONTROL_LOW_LIGHT,
				fLowLight->Value() == B_CONTROL_ON);
			_UpdateDefaultsButton();
			_ScheduleSave();
			break;

		case kMsgDefaults:
			for (int32 i = 0; i < 4; i++) {
				fLevels[i]->SetValue(0);
				_SetControl(fLevels[i]->Control(), kDriverMiddle);
			}
			fWhiteBalance->SetValue(B_CONTROL_ON);
			_SetControl(FTHD_CONTROL_AUTO_WHITE_BALANCE, 1);
			fLowLight->SetValue(B_CONTROL_OFF);
			_SetControl(FTHD_CONTROL_LOW_LIGHT, 0);
			_UpdateDefaultsButton();
			_ScheduleSave();
			break;

		case kMsgUpdateStatus:
			_UpdateStatus();
			break;

		case kMsgSave:
			delete fSaveRunner;
			fSaveRunner = NULL;
			_SaveSettings();
			break;

		default:
			BWindow::MessageReceived(message);
			break;
	}
}


status_t
CameraWindow::_Control(uint32 op, uint32 id, int32* value)
{
	if (fDevice < 0)
		return B_NO_INIT;

	fthd_control control = { id, *value };
	if (ioctl(fDevice, op, &control, sizeof(control)) != 0)
		return errno;
	*value = control.value;
	return B_OK;
}


void
CameraWindow::_Load()
{
	// Opening the device does not turn the camera on.
	fDevice = open(kDevice, O_RDWR);

	bool available = fDevice >= 0;
	for (int32 i = 0; i < 4; i++) {
		int32 value = kDriverMiddle;
		if (available)
			_Control(FTHD_GET_CONTROL, fLevels[i]->Control(), &value);
		fLevels[i]->SetValue(level_for_driver(value));
		fLevels[i]->SetEnabled(available);
	}

	int32 value = 1;
	if (available)
		_Control(FTHD_GET_CONTROL, FTHD_CONTROL_AUTO_WHITE_BALANCE, &value);
	fWhiteBalance->SetValue(value != 0 ? B_CONTROL_ON : B_CONTROL_OFF);
	fWhiteBalance->SetEnabled(available);

	value = 0;
	if (available)
		_Control(FTHD_GET_CONTROL, FTHD_CONTROL_LOW_LIGHT, &value);
	fLowLight->SetValue(value != 0 ? B_CONTROL_ON : B_CONTROL_OFF);
	fLowLight->SetEnabled(available);

	_UpdateStatus();
	_UpdateDefaultsButton();
}


void
CameraWindow::_SetControl(uint32 id, int32 value)
{
	if (_Control(FTHD_SET_CONTROL, id, &value) != B_OK)
		_UpdateStatus();
}


void
CameraWindow::_ScheduleSave()
{
	if (fDevice < 0)
		return;

	fSavePending = true;
	delete fSaveRunner;
	BMessage save(kMsgSave);
	fSaveRunner = new BMessageRunner(BMessenger(this), &save, kSaveDelay, 1);
}


/*!	Writes the current settings to the driver's settings file (see
	facetimehd_ioctl.h), through a temporary file so a crash never leaves a
	half-written one behind.
*/
status_t
CameraWindow::_SaveSettings()
{
	fSavePending = false;

	int32 values[FTHD_CONTROL_COUNT];
	for (int32 i = 0; i < 4; i++)
		values[fLevels[i]->Control()] = driver_for_level(fLevels[i]->Value());
	values[FTHD_CONTROL_AUTO_WHITE_BALANCE]
		= fWhiteBalance->Value() == B_CONTROL_ON ? 1 : 0;
	values[FTHD_CONTROL_LOW_LIGHT] = fLowLight->Value() == B_CONTROL_ON ? 1 : 0;

	BString text("# FaceTime HD camera picture settings, written by the "
		"Camera preferences.\n# The facetimehd driver reads them when it "
		"loads.\n");
	for (uint32 id = 0; id < FTHD_CONTROL_COUNT; id++)
		text << kFTHDControlNames[id] << " " << values[id] << "\n";

	BPath path;
	status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, &path, true);
	if (status == B_OK)
		status = path.Append("kernel/drivers");
	if (status == B_OK)
		status = create_directory(path.Path(), 0755);
	if (status != B_OK)
		return status;

	BPath temporary(path.Path(), FTHD_SETTINGS_NAME "~");
	BPath final(path.Path(), FTHD_SETTINGS_NAME);
	BFile file(temporary.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	status = file.InitCheck();
	if (status == B_OK) {
		ssize_t written = file.Write(text.String(), text.Length());
		if (written != text.Length())
			status = written < 0 ? written : B_IO_ERROR;
	}
	if (status == B_OK)
		status = file.Sync();
	file.Unset();
	if (status == B_OK && rename(temporary.Path(), final.Path()) != 0)
		status = errno;

	// Shown on the status line until the next save.
	if (status == B_OK)
		fSaveState = "Settings saved";
	else
		fSaveState.SetToFormat("Could not save the settings: %s", strerror(status));
	_UpdateStatus();
	return status;
}


void
CameraWindow::_UpdateStatus()
{
	if (fDevice < 0) {
		fStatus->SetText("No FaceTime HD camera found.");
		return;
	}

	fthd_info info;
	if (ioctl(fDevice, FTHD_GET_INFO, &info, sizeof(info)) != 0
		|| info.version != FTHD_IOCTL_VERSION) {
		fStatus->SetText("The camera driver does not match this app.");
		return;
	}

	BString text;
	text.SetToFormat("%s  \xC2\xB7  %s", info.streaming ? "On" : "Off",
		info.set_file_loaded ? "Colour calibrated"
			: "Colour calibration file missing");
	if (!fSaveState.IsEmpty())
		text << "  \xC2\xB7  " << fSaveState;
	fStatus->SetText(text);
}


void
CameraWindow::_UpdateDefaultsButton()
{
	bool isDefault = fWhiteBalance->Value() == B_CONTROL_ON
		&& fLowLight->Value() == B_CONTROL_OFF;
	for (int32 i = 0; i < 4; i++) {
		if (fLevels[i]->Value() != 0)
			isDefault = false;
	}
	fDefaultsButton->SetEnabled(fDevice >= 0 && !isDefault);
}
