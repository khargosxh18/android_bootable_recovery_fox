/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.

	OrangeFox is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#include "fox_screen_service.hpp"

#include <time.h>

#include "fox_screen.hpp"

int Fox_Screen_Service::Width()
{
	return Fox_Screen::Width();
}

int Fox_Screen_Service::Height()
{
	return Fox_Screen::Height();
}

void Fox_Screen_Service::RequestFrames()
{
	Fox_Screen::RequestFrames();
}

bool Fox_Screen_Service::GetLatest(std::string& png, uint64_t* generation)
{
	return Fox_Screen::GetLatest(png, generation);
}

bool Fox_Screen_Service::CaptureRenderedFrameNow(std::string& png)
{
	return Fox_Screen::CaptureFrameNow(png);
}

bool Fox_Screen_Service::CaptureRenderedFrameIfWanted()
{
	Fox_Screen::CaptureFrameIfWanted();
	return true;
}

bool Fox_Screen_Service::ShouldForceRender()
{
	return Fox_Screen::ShouldForceRender();
}

bool Fox_Screen_Service::GetFreshPng(std::string& png, int attempts, int wait_ms)
{
	uint64_t start_gen = 0;
	bool had_frame = GetLatest(png, &start_gen);
	if (attempts < 1)
		attempts = 1;
	if (wait_ms < 0)
		wait_ms = 0;

	for (int i = 0; i < attempts; i++) {
		RequestFrames();
		std::string next;
		uint64_t gen = 0;
		if (GetLatest(next, &gen)) {
			if (!had_frame || gen != start_gen) {
				png.swap(next);
				return true;
			}
			if (png.empty())
				png.swap(next);
		}
		struct timespec ts = { wait_ms / 1000, (wait_ms % 1000) * 1000 * 1000 };
		nanosleep(&ts, nullptr);
	}

	std::string latest;
	uint64_t latest_gen = 0;
	if (GetLatest(latest, &latest_gen) && (!had_frame || latest_gen != start_gen))
		png.swap(latest);
	return !png.empty();
}
