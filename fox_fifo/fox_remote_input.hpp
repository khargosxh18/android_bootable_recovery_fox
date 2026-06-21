/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.

	OrangeFox is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#ifndef _FOX_REMOTE_INPUT_HPP
#define _FOX_REMOTE_INPUT_HPP

#include <cstdio>
#include <string>

#include <json/json.h>

class Fox_Remote_Input
{
public:
	static bool Init();
	static void Shutdown();
	static bool IsReady();

	static int KeyCodeFromName(const std::string& name);
	static void Tap(int x, int y);
	static void TouchDown(int x, int y);
	static void TouchMove(int x, int y);
	static void TouchUp();
	static void Swipe(int x1, int y1, int x2, int y2, int ms);
	static void Key(int code, bool down);
	static void KeyTap(int code);

	// Execute an input request from its typed args ({action, key, code, x, y,
	// x2, y2, duration}). Diagnostics are appended to *output.
	static int RunCommand(const Json::Value& args, std::string* output);
};

#endif // _FOX_REMOTE_INPUT_HPP
