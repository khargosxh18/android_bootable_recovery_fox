/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.

	OrangeFox is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#include "fox_channel.hpp"

#include <cerrno>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "fox_command_dispatcher.hpp"
#include "fox_protocol.hpp"
#include "../data.hpp"
#include "../orscmd/orscmd.h"
#include "../twcommon.h"
#include "../gui/gui.h"

extern void set_select_fd();

namespace {

int g_input_fd = -1;
int g_cancel_fd = -1;

void setup_command_fifo()
{
	g_input_fd = -1;
	set_select_fd();

	unlink(FOX_INPUT_FILE);
	if (mkfifo(FOX_INPUT_FILE, 06660) != 0) {
		LOGINFO("Unable to mkfifo %s\n", FOX_INPUT_FILE);
		return;
	}
	unlink(FOX_OUTPUT_FILE);
	if (mkfifo(FOX_OUTPUT_FILE, 06666) != 0) {
		LOGINFO("Unable to mkfifo %s\n", FOX_OUTPUT_FILE);
		unlink(FOX_INPUT_FILE);
		return;
	}

	g_input_fd = open(FOX_INPUT_FILE, O_RDONLY | O_NONBLOCK);
	if (g_input_fd < 0) {
		LOGINFO("Unable to open %s\n", FOX_INPUT_FILE);
		unlink(FOX_INPUT_FILE);
		unlink(FOX_OUTPUT_FILE);
	}
	set_select_fd();
}

void setup_cancel_fifo()
{
	if (g_cancel_fd >= 0) {
		close(g_cancel_fd);
		g_cancel_fd = -1;
	}
	unlink(FOX_CANCEL_FILE);
	if (mkfifo(FOX_CANCEL_FILE, 06660) != 0) {
		LOGINFO("Unable to mkfifo %s\n", FOX_CANCEL_FILE);
		set_select_fd();
		return;
	}
	g_cancel_fd = open(FOX_CANCEL_FILE, O_RDONLY | O_NONBLOCK);
	if (g_cancel_fd < 0) {
		LOGINFO("Unable to open %s\n", FOX_CANCEL_FILE);
		unlink(FOX_CANCEL_FILE);
	}
	set_select_fd();
}

void close_input_fd()
{
	if (g_input_fd >= 0) {
		close(g_input_fd);
		g_input_fd = -1;
	}
}

} // namespace

void Fox_Channel::Setup()
{
	if (g_input_fd < 0)
		setup_command_fifo();
	if (g_cancel_fd < 0)
		setup_cancel_fifo();
}

void Fox_Channel::Shutdown()
{
	close_input_fd();
	if (g_cancel_fd >= 0) {
		close(g_cancel_fd);
		g_cancel_fd = -1;
	}
	Fox_Command_Dispatcher::ResetPendingIfIdle();
	set_select_fd();
}

void Fox_Channel::RearmInput()
{
	close_input_fd();
	setup_command_fifo();
}

int Fox_Channel::InputFd()
{
	return g_input_fd;
}

int Fox_Channel::CancelFd()
{
	return g_cancel_fd;
}

bool Fox_Channel::IsActive()
{
	return Fox_Command_Dispatcher::IsActive();
}

void Fox_Channel::HandleInput()
{
	// Accumulate the whole request rather than capping at a single 4096-byte
	// read — a JSON request (many zip paths, long passwords) can exceed that.
	// The fd is non-blocking, so drain until EOF (writer closed) or EAGAIN.
	static const size_t kMaxRequestBytes = 256 * 1024;
	std::string buffer;
	char chunk[4096];
	bool saw_eof = false;
	for (;;) {
		int n = read(g_input_fd, chunk, sizeof(chunk));
		if (n > 0) {
			buffer.append(chunk, (size_t)n);
			if (buffer.size() > kMaxRequestBytes)
				break;  // runaway/oversized request: stop and let parsing reject it
			continue;
		}
		if (n == 0) {
			saw_eof = true;
			break;
		}
		if (errno == EINTR)
			continue;
		break;  // EAGAIN/EWOULDBLOCK or error: nothing more available right now
	}

	if (buffer.empty()) {
		if (saw_eof)
			RearmInput();
		return;
	}

	Fox_Rpc_Request request = Fox_Protocol::ParseRequestJson(buffer);

	FILE* out = fopen(FOX_OUTPUT_FILE, "w");
	if (!out) {
		close_input_fd();
		set_select_fd();
		LOGINFO("Unable to fopen %s\n", FOX_OUTPUT_FILE);
		unlink(FOX_INPUT_FILE);
		unlink(FOX_OUTPUT_FILE);
		return;
	}
	if (!request.ok()) {
		Fox_Protocol::WriteErrorEvent(out, request.id, request.error, "invalid fox rpc request");
		Fox_Protocol::WriteResultEvent(out, request.id, 2);
		fclose(out);
		RearmInput();
		return;
	}

	LOGINFO("fox rpc '%s' received\n", request.op.c_str());

	if (DataManager::GetIntValue("tw_busy") != 0) {
		Fox_Protocol::WriteErrorEvent(out, request.id, "busy", "operation in progress");
		Fox_Protocol::WriteResultEvent(out, request.id, 1);
		LOGINFO("fox command cannot be performed, operation in progress.\n");
		fclose(out);
		RearmInput();
		return;
	}

	Fox_Command_Dispatcher::Result result = Fox_Command_Dispatcher::DispatchFifo(out, request);
	if (result == Fox_Command_Dispatcher::FAILED) {
		fclose(out);
		RearmInput();
	}
}

bool Fox_Channel::HandleCancel(bool legacy_command_active)
{
	char buffer[64];
	int read_ret = read(g_cancel_fd, &buffer, sizeof(buffer));
	if (read_ret <= 0) {
		setup_cancel_fifo();
		return false;
	}
	return Fox_Command_Dispatcher::CancelRequested(legacy_command_active);
}

bool Fox_Channel::CapturePendingFrameIfNeeded()
{
	return Fox_Command_Dispatcher::CapturePendingFrameIfNeeded();
}
