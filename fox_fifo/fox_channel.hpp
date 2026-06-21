/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.

	OrangeFox is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#ifndef _FOX_CHANNEL_HPP
#define _FOX_CHANNEL_HPP

#include <cstdio>
#include <string>
#include <vector>

class Fox_Channel
{
public:
	static void Setup();
	static void Shutdown();
	static void RearmInput();

	static int InputFd();
	static int CancelFd();
	static bool IsActive();

	static void HandleInput();
	static bool HandleCancel(bool legacy_command_active = false);

	// Called on the GUI thread after PageManager::Render() and before flip().
	static bool CapturePendingFrameIfNeeded();

};

#endif // _FOX_CHANNEL_HPP
