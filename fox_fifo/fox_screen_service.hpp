/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.

	OrangeFox is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#ifndef _FOX_SCREEN_SERVICE_HPP
#define _FOX_SCREEN_SERVICE_HPP

#include <cstdint>
#include <string>

class Fox_Screen_Service
{
public:
	static int Width();
	static int Height();

	static void RequestFrames();
	static bool GetLatest(std::string& png, uint64_t* generation = nullptr);
	static bool CaptureRenderedFrameNow(std::string& png);
	static bool CaptureRenderedFrameIfWanted();
	static bool ShouldForceRender();

	static bool GetFreshPng(std::string& png, int attempts = 8, int wait_ms = 20);
};

#endif // _FOX_SCREEN_SERVICE_HPP
