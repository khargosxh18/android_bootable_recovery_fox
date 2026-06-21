/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.

	OrangeFox is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#ifndef _FOX_COMMAND_DISPATCHER_HPP
#define _FOX_COMMAND_DISPATCHER_HPP

#include <cstdio>
#include <string>
#include <vector>

#include <json/json.h>

#include "fox_protocol.hpp"

class Fox_Command_Dispatcher
{
public:
	enum Result {
		FINISHED,
		ACTIVE,
		FAILED
	};

	static bool IsActive();

	// Emit a structured `data` event (named 'name', payload 'payload') for the
	// active command — the uniform way query/list handlers return results.
	// When no RPC transport is active (e.g. a GUI-invoked command) the JSON is
	// printed to the console instead.
	static void EmitData(const std::string& name, const Json::Value& payload);
	static Result DispatchFifo(FILE* out, const Fox_Rpc_Request& request);
	static bool DispatchJob(FILE* out, const Fox_Rpc_Request& request);
	static bool CapturePendingFrameIfNeeded();
	static bool CancelRequested(bool legacy_command_active);
	static void CommandDone(int code = 0);
	static void ResetPendingIfIdle();
};

#endif // _FOX_COMMAND_DISPATCHER_HPP
