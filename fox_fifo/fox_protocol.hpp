/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.

	OrangeFox is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#ifndef _FOX_PROTOCOL_HPP
#define _FOX_PROTOCOL_HPP

#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include <json/json.h>

struct Fox_Rpc_Request
{
	int version = 0;
	std::string id;
	std::string op;
	Json::Value args;
	std::string error;
	std::string raw;

	bool ok() const { return error.empty(); }
};

class Fox_Protocol
{
public:
	static std::string Base64Encode(const std::string& in, bool newline = true);
	static Fox_Rpc_Request ParseRequestJson(const char* buffer, int len);
	static Fox_Rpc_Request ParseRequestJson(const std::string& json);

	static void WriteResultEvent(FILE* out, const std::string& id, int code);
	static void WriteErrorEvent(FILE* out, const std::string& id, const std::string& code, const std::string& message);
	static void WriteLogEvent(FILE* out, const std::string& id, const char* data, size_t size);
	static void WriteProgressEvent(FILE* out, const std::string& id, const std::string& phase, int percent);
	static void WriteDataEvent(FILE* out, const std::string& id, const std::string& name, const std::string& json);
	// Overload taking an already-built value, avoiding a serialize→reparse round
	// trip when the caller already has the payload as a Json::Value.
	static void WriteDataEvent(FILE* out, const std::string& id, const std::string& name, const Json::Value& value);
	static FILE* OpenEventLogFile(FILE* out, const std::string& id);
	static void WriteKvBlock(FILE* out, const char* begin, const std::map<std::string, std::string>& kv, const char* end);

	static int ClampFps(int fps);
	static std::string JsonEscape(const std::string& in);
	static std::string JsonString(const Json::Value& value);
};

#endif // _FOX_PROTOCOL_HPP
