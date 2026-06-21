/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.

	OrangeFox is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#include "fox_command_dispatcher.hpp"

#include "fox_channel.hpp"
#include "fox_fifo.hpp"
#include "fox_protocol.hpp"
#include "fox_remote_input.hpp"
#include "fox_screen_service.hpp"
#include "fox_screen_stream.hpp"
#include "../data.hpp"
#include "../gui/gui.h"
#include "../gui/gui.hpp"
#include "../gui/objects.hpp"
#include "../gui/pages.hpp"
#include "../twcommon.h"

namespace {

class CommandOutput
{
public:
	CommandOutput() : transport_(nullptr), log_(nullptr), fifo_(false) {}
	~CommandOutput()
	{
		Close();
	}

	bool Reset(FILE* file, const std::string& id, bool fifo)
	{
		Close();
		transport_ = file;
		id_ = id;
		fifo_ = fifo;
		log_ = Fox_Protocol::OpenEventLogFile(transport_, id_);
		if (!log_) {
			transport_ = nullptr;
			id_.clear();
			fifo_ = false;
			return false;
		}
		return true;
	}

	FILE* transport() const
	{
		return transport_;
	}

	FILE* log() const
	{
		return log_;
	}

	const std::string& id() const
	{
		return id_;
	}

	bool active() const
	{
		return transport_ != nullptr;
	}

	void Finish(int code)
	{
		if (!transport_)
			return;
		bool was_fifo = fifo_;
		CloseLog();  // flush trailing log output before the result event
		Fox_Protocol::WriteResultEvent(transport_, id_, code);
		Close();
		Fox_Channel::RearmInput();
	}

	// Close just the log stream. fclose() runs the funopen close callback, which
	// emits any buffered partial (newline-less) line as a log event. Call this
	// before writing the result event so trailing output can't arrive after it.
	void CloseLog()
	{
		gui_set_FILE(nullptr);
		if (log_) {
			fclose(log_);
			log_ = nullptr;
		}
	}

	void Close()
	{
		CloseLog();
		if (transport_) {
			fclose(transport_);
			transport_ = nullptr;
		}
		id_.clear();
		fifo_ = false;
	}

private:
	FILE* transport_;
	FILE* log_;
	std::string id_;
	bool fifo_;
};

CommandOutput g_output;
bool g_pending_screencap = false;

// The positional control verbs (screenstream/input/base64-screencap) are
// handled inline from the typed request rather than going through the GUI
// action path, so screen/input traffic stays off singleaction_page.
enum class ControlResult { NotControl, Finished, Active };

ControlResult dispatch_control(const Fox_Rpc_Request& request)
{
	if (request.op == "screenstream") {
		int code = Fox_Screen_Stream::HandleCommand(request.args, g_output.log());
		g_output.Finish(code);
		return ControlResult::Finished;
	}
	if (request.op == "input") {
		std::string output;
		int code = Fox_Remote_Input::RunCommand(request.args, &output);
		if (!output.empty())
			Fox_Protocol::WriteLogEvent(g_output.transport(), g_output.id(), output.data(), output.size());
		g_output.Finish(code);
		return ControlResult::Finished;
	}
	if (request.op == "screencap") {
		// Only the base64 (inline) form is intercepted; a plain screencap that
		// writes a PNG file goes through the normal engine handler.
		const Json::Value& b = request.args["base64"];
		if (b.isBool() && b.asBool()) {
			g_pending_screencap = true;
			gui_forceRender();
			return ControlResult::Active;
		}
	}
	return ControlResult::NotControl;
}

void start_action(const Fox_Rpc_Request& request, bool progress)
{
	Fox_Fifo::Set_Request(request.op, request.args);
	gui_set_FILE(g_output.log());
	if (progress)
		gui_fox_progress_begin();
	DataManager::SetValue("tw_action", "foxcmd");
	DataManager::SetValue("tw_action_param", "");
	std::string currentPage = PageManager::GetCurrentPage();
	DataManager::SetValue("tw_has_action2", "1");
	DataManager::SetValue("tw_action2", "page");
	DataManager::SetValue("tw_action2_param", currentPage);
	DataManager::SetValue("tw_action_text1", gui_lookup("running_recovery_commands", "Running Recovery Commands"));
	DataManager::SetValue("tw_action_text2", "");
	gui_changePage("singleaction_page");
}

} // namespace

bool Fox_Command_Dispatcher::IsActive()
{
	return g_output.active();
}

void Fox_Command_Dispatcher::EmitData(const std::string& name, const Json::Value& payload)
{
	if (g_output.active()) {
		// Preserve ordering with any buffered log output before the data event.
		if (g_output.log())
			fflush(g_output.log());
		Fox_Protocol::WriteDataEvent(g_output.transport(), g_output.id(),
		                             name, payload);
	} else {
		// No RPC transport (e.g. a GUI-invoked command): show the JSON inline.
		gui_print("%s\n", Fox_Protocol::JsonString(payload).c_str());
	}
}

Fox_Command_Dispatcher::Result Fox_Command_Dispatcher::DispatchFifo(FILE* out, const Fox_Rpc_Request& request)
{
	if (!out || IsActive())
		return FAILED;

	if (!g_output.Reset(out, request.id, true))
		return FAILED;

	if (request.op.empty()) {
		Fox_Protocol::WriteErrorEvent(g_output.transport(), request.id, "unknown_op", "unsupported fox rpc operation");
		g_output.Finish(2);
		return FINISHED;
	}

	switch (dispatch_control(request)) {
		case ControlResult::Finished:
			return FINISHED;
		case ControlResult::Active:
			return ACTIVE;
		case ControlResult::NotControl:
			break;
	}

	start_action(request, true);
	return ACTIVE;
}

bool Fox_Command_Dispatcher::DispatchJob(FILE* out, const Fox_Rpc_Request& request)
{
	if (!out || IsActive())
		return false;
	if (!g_output.Reset(out, request.id, false))
		return false;
	if (request.op.empty()) {
		Fox_Protocol::WriteErrorEvent(g_output.transport(), request.id, "unknown_op", "unsupported fox rpc operation");
		g_output.Finish(2);
		return true;
	}

	switch (dispatch_control(request)) {
		case ControlResult::Finished:
		case ControlResult::Active:
			return true;
		case ControlResult::NotControl:
			break;
	}

	start_action(request, false);
	return true;
}

bool Fox_Command_Dispatcher::CapturePendingFrameIfNeeded()
{
	if (!g_pending_screencap)
		return false;

	std::string png;
	if (Fox_Screen_Service::CaptureRenderedFrameNow(png)) {
		Json::Value data(Json::objectValue);
		data["mime"] = "image/png";
		data["base64"] = Fox_Protocol::Base64Encode(png, false);
		Fox_Protocol::WriteDataEvent(g_output.transport(), g_output.id(), "screencap", data);
		g_pending_screencap = false;
		g_output.Finish(0);
	} else {
		Fox_Protocol::WriteErrorEvent(g_output.transport(), g_output.id(), "screencap_failed", "screencap failed");
		g_pending_screencap = false;
		g_output.Finish(1);
	}
	return true;
}

bool Fox_Command_Dispatcher::CancelRequested(bool legacy_command_active)
{
	if (Fox_Screen_Stream::IsActive()) {
		LOGINFO("fox: screen stream cancel request received\n");
		Fox_Screen_Stream::Stop();
	}
	if (IsActive() || legacy_command_active) {
		LOGINFO("fox: cancel request received\n");
		return true;
	}
	return false;
}

void Fox_Command_Dispatcher::CommandDone(int code)
{
	gui_fox_progress_end();
	g_output.CloseLog();  // flush trailing log output before the result event
	if (g_output.transport())
		Fox_Protocol::WriteResultEvent(g_output.transport(), g_output.id(), code);
	g_output.Close();

	if (DataManager::GetIntValue("tw_page_done") == 0)
		Fox_Channel::RearmInput();
}

void Fox_Command_Dispatcher::ResetPendingIfIdle()
{
	if (!IsActive())
		g_pending_screencap = false;
}

void fox_command_done()
{
	Fox_Command_Dispatcher::CommandDone();
}
