/*
 * Copyright 2026, RenkuOS Contributors.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 */


//!	Camera: picture settings for the FaceTime HD camera.


#include <Application.h>

#include "CameraWindow.h"


int
main()
{
	BApplication app("application/x-vnd.RenkuOS-Camera");
	(new CameraWindow())->Show();
	app.Run();
	return 0;
}
