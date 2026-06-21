/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.

	OrangeFox is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#include "fox_remote_input.hpp"

#include <cctype>
#include <cstdlib>
#include <linux/input.h>
#include <string>
#include <time.h>

#include "fox_input.hpp"

bool Fox_Remote_Input::Init()
{
	return Fox_Input::Init();
}

void Fox_Remote_Input::Shutdown()
{
	Fox_Input::Shutdown();
}

bool Fox_Remote_Input::IsReady()
{
	return Fox_Input::IsReady();
}

int Fox_Remote_Input::KeyCodeFromName(const std::string& name)
{
	if (name == "back")    return KEY_BACK;
	if (name == "home")    return KEY_HOMEPAGE;
	if (name == "menu")    return KEY_MENU;
	if (name == "power")   return KEY_POWER;
	if (name == "volup")   return KEY_VOLUMEUP;
	if (name == "voldown") return KEY_VOLUMEDOWN;
	if (name == "enter")   return KEY_ENTER;
	if (name == "up")      return KEY_UP;
	if (name == "down")    return KEY_DOWN;
	if (name == "left")    return KEY_LEFT;
	if (name == "right")   return KEY_RIGHT;
	return -1;
}

void Fox_Remote_Input::Tap(int x, int y)
{
	Fox_Input::Tap(x, y);
}

void Fox_Remote_Input::TouchDown(int x, int y)
{
	Fox_Input::TouchDown(x, y);
}

void Fox_Remote_Input::TouchMove(int x, int y)
{
	Fox_Input::TouchMove(x, y);
}

void Fox_Remote_Input::TouchUp()
{
	Fox_Input::TouchUp();
}

void Fox_Remote_Input::Swipe(int x1, int y1, int x2, int y2, int ms)
{
	if (ms < 0)
		ms = 0;
	const int steps = 16;
	TouchDown(x1, y1);
	for (int i = 1; i <= steps; i++) {
		int x = x1 + (x2 - x1) * i / steps;
		int y = y1 + (y2 - y1) * i / steps;
		TouchMove(x, y);
		struct timespec ts = { 0, (long)ms * 1000000L / steps };
		nanosleep(&ts, nullptr);
	}
	TouchUp();
}

void Fox_Remote_Input::Key(int code, bool down)
{
	Fox_Input::Key(code, down);
}

void Fox_Remote_Input::KeyTap(int code)
{
	Fox_Input::KeyTap(code);
}

namespace {

void append_line(std::string* out, const std::string& line)
{
	if (out)
		*out += line + "\n";
}

} // namespace

namespace {

// Integer field accessor: number or numeric-string, else 'dflt'.
int json_int(const Json::Value& args, const char* key, int dflt = 0)
{
	if (!args.isObject() || !args.isMember(key))
		return dflt;
	const Json::Value& v = args[key];
	if (v.isIntegral())
		return (int)v.asInt64();
	if (v.isString())
		return atoi(v.asString().c_str());
	return dflt;
}

std::string json_str(const Json::Value& args, const char* key)
{
	if (!args.isObject() || !args.isMember(key))
		return "";
	const Json::Value& v = args[key];
	return v.isString() ? v.asString() : "";
}

// True when 'key' is present as a usable integer (number or numeric string).
bool json_has_int(const Json::Value& args, const char* key)
{
	if (!args.isObject() || !args.isMember(key))
		return false;
	const Json::Value& v = args[key];
	return v.isIntegral() || v.isString();
}

} // namespace

int Fox_Remote_Input::RunCommand(const Json::Value& args, std::string* output)
{
	std::string sub = json_str(args, "action");
	if (sub.empty()) {
		append_line(output, "fox: input requires a subcommand (tap|swipe|key|down|move|up)");
		return 2;
	}
	if (!Init()) {
		append_line(output, "fox: unable to create virtual input device");
		return 1;
	}

	int x = json_int(args, "x");
	int y = json_int(args, "y");
	// tap/down/move target a point: reject rather than silently injecting at (0,0)
	// when coordinates are missing.
	if (sub == "tap" || sub == "down" || sub == "move") {
		if (!json_has_int(args, "x") || !json_has_int(args, "y")) {
			append_line(output, "fox: input " + sub + " requires x and y");
			return 2;
		}
		if (sub == "tap")
			Tap(x, y);
		else if (sub == "down")
			TouchDown(x, y);
		else
			TouchMove(x, y);
		return 0;
	}
	if (sub == "up") {
		TouchUp();
		return 0;
	}
	if (sub == "swipe") {
		if (!json_has_int(args, "x") || !json_has_int(args, "y") ||
		    !json_has_int(args, "x2") || !json_has_int(args, "y2")) {
			append_line(output, "fox: input swipe requires x, y, x2 and y2");
			return 2;
		}
		int ms = json_int(args, "duration", 200);
		if (ms <= 0)
			ms = 200;
		Swipe(x, y, json_int(args, "x2"), json_int(args, "y2"), ms);
		return 0;
	}
	if (sub == "key") {
		std::string name = json_str(args, "key");
		int code = -1;
		if (!name.empty())
			code = isdigit((unsigned char)name[0]) ? atoi(name.c_str()) : KeyCodeFromName(name);
		else if (args.isObject() && args["code"].isIntegral())
			code = (int)args["code"].asInt64();
		if (code < 0) {
			append_line(output, "fox: unknown key '" + name + "'");
			return 2;
		}
		KeyTap(code);
		return 0;
	}

	append_line(output, "fox: unknown input subcommand '" + sub + "' (tap|swipe|key|down|move|up)");
	return 2;
}
