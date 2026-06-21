/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.

	OrangeFox is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#ifndef _FOX_SCREEN_STREAM_HPP
#define _FOX_SCREEN_STREAM_HPP

#include <cstdio>
#include <string>

#include <json/json.h>

class Fox_Screen_Stream
{
public:
	static bool IsActive();
	static bool ShouldForceRender();

	// Called on the GUI thread after PageManager::Render() and before flip().
	// Captures/submits a frame if the active stream is due.
	static bool CaptureRenderedFrameIfDue();

	static int Start(int fps);
	static void Stop();
	// Handle a typed `screenstream` request ({action, fps}); writes its reply to
	// 'out' and returns the result code. Called only for op == "screenstream".
	static int HandleCommand(const Json::Value& args, FILE* out);
};

#endif // _FOX_SCREEN_STREAM_HPP
