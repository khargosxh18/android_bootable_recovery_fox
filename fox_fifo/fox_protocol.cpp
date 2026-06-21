/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.

	OrangeFox is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#include "fox_protocol.hpp"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>

#include "../orscmd/orscmd.h"

namespace {

struct EventLogCookie {
	FILE* out;
	std::string id;
	std::string pending;
};

std::string write_json(const Json::Value& value)
{
	Json::StreamWriterBuilder builder;
	builder["indentation"] = "";
	builder["commentStyle"] = "None";
	return Json::writeString(builder, value);
}

void add_id(Json::Value& value, const std::string& id)
{
	if (!id.empty())
		value["id"] = id;
}

bool write_progress_line(FILE* out, const std::string& id, const char* buf, int size)
{
	static const char prefix[] = "FOX_PROGRESS\t";
	std::string line(buf, (size_t)size);
	if (line.compare(0, sizeof(prefix) - 1, prefix) != 0)
		return false;

	while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
		line.pop_back();

	std::map<std::string, std::string> fields;
	size_t pos = sizeof(prefix) - 1;
	while (pos < line.size()) {
		size_t next = line.find('\t', pos);
		std::string field = line.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
		size_t eq = field.find('=');
		if (eq != std::string::npos)
			fields[field.substr(0, eq)] = field.substr(eq + 1);
		if (next == std::string::npos)
			break;
		pos = next + 1;
	}

	if (fields.empty())
		return true;

	std::string phase = fields.count("phase") ? fields["phase"] : "overall";
	int percent = fields.count("percent") ? atoi(fields["percent"].c_str()) : -1;
	if (fields.count("item") && !fields.count("phase")) {
		phase = "item";
		percent = atoi(fields["item"].c_str());
	}
	if (percent < 0)
		percent = 0;
	if (percent > 100)
		percent = 100;

	Json::Value event(Json::objectValue);
	add_id(event, id);
	event["event"] = "progress";
	event["phase"] = phase;
	event["percent"] = percent;

	static const char* numeric_fields[] = {
		"current_bytes", "total_bytes", "bytes_per_second", "eta_seconds",
		"current_files", "total_files"
	};
	for (const char* key : numeric_fields) {
		std::map<std::string, std::string>::const_iterator it = fields.find(key);
		if (it != fields.end())
			event[key] = Json::UInt64(strtoull(it->second.c_str(), nullptr, 10));
	}
	static const char* string_fields[] = { "label", "size_text", "file_text" };
	for (const char* key : string_fields) {
		std::map<std::string, std::string>::const_iterator it = fields.find(key);
		if (it != fields.end() && !it->second.empty())
			event[key] = it->second;
	}

	std::string json = Fox_Protocol::JsonString(event);
	fprintf(out, "%s\n", json.c_str());
	return true;
}

int event_write(void* cookie, const char* buf, int size)
{
	EventLogCookie* log = static_cast<EventLogCookie*>(cookie);
	log->pending.append(buf, (size_t)size);
	size_t start = 0;
	for (;;) {
		size_t end = log->pending.find('\n', start);
		if (end == std::string::npos)
			break;
		size_t len = end + 1 - start;
		const char* line = log->pending.data() + start;
		if (!write_progress_line(log->out, log->id, line, (int)len))
			Fox_Protocol::WriteLogEvent(log->out, log->id, line, len);
		start = end + 1;
	}
	if (start > 0)
		log->pending.erase(0, start);
	return size;
}

int event_close(void* cookie)
{
	EventLogCookie* log = static_cast<EventLogCookie*>(cookie);
	if (!log->pending.empty()) {
		if (!write_progress_line(log->out, log->id, log->pending.data(), (int)log->pending.size()))
			Fox_Protocol::WriteLogEvent(log->out, log->id, log->pending.data(), log->pending.size());
	}
	delete log;
	return 0;
}

} // namespace

std::string Fox_Protocol::Base64Encode(const std::string& in, bool newline)
{
	static const char table[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve(((in.size() + 2) / 3) * 4 + (newline ? 1 : 0));

	for (size_t i = 0; i < in.size(); i += 3) {
		unsigned int b0 = (unsigned char)in[i];
		unsigned int b1 = i + 1 < in.size() ? (unsigned char)in[i + 1] : 0;
		unsigned int b2 = i + 2 < in.size() ? (unsigned char)in[i + 2] : 0;
		out += table[b0 >> 2];
		out += table[((b0 & 0x03) << 4) | (b1 >> 4)];
		out += i + 1 < in.size() ? table[((b1 & 0x0f) << 2) | (b2 >> 6)] : '=';
		out += i + 2 < in.size() ? table[b2 & 0x3f] : '=';
	}
	if (newline)
		out += '\n';
	return out;
}

Fox_Rpc_Request Fox_Protocol::ParseRequestJson(const char* buffer, int len)
{
	return ParseRequestJson(std::string(buffer, len));
}

Fox_Rpc_Request Fox_Protocol::ParseRequestJson(const std::string& json)
{
	Fox_Rpc_Request request;
	request.raw = json;

	Json::CharReaderBuilder builder;
	builder["collectComments"] = false;
	Json::Value root;
	std::string errors;
	std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
	if (!reader->parse(json.data(), json.data() + json.size(), &root, &errors)) {
		request.error = "malformed_json";
		return request;
	}
	if (!root.isObject()) {
		request.error = "request_not_object";
		return request;
	}
	if (!root["v"].isInt()) {
		request.error = "missing_version";
		return request;
	}
	request.version = root["v"].asInt();
	if (request.version != 1) {
		request.error = "unsupported_version";
		return request;
	}
	if (root["id"].isString())
		request.id = root["id"].asString();
	if (!root["op"].isString() || root["op"].asString().empty()) {
		request.error = "missing_op";
		return request;
	}
	request.op = root["op"].asString();
	if (root.isMember("args")) {
		if (!root["args"].isObject()) {
			request.error = "args_not_object";
			return request;
		}
		request.args = root["args"];
	} else {
		request.args = Json::Value(Json::objectValue);
	}
	return request;
}

void Fox_Protocol::WriteResultEvent(FILE* out, const std::string& id, int code)
{
	if (!out)
		return;
	Json::Value event(Json::objectValue);
	add_id(event, id);
	event["event"] = "result";
	event["code"] = code;
	std::string line = JsonString(event);
	fprintf(out, "%s\n", line.c_str());
}

void Fox_Protocol::WriteErrorEvent(FILE* out, const std::string& id, const std::string& code, const std::string& message)
{
	if (!out)
		return;
	Json::Value event(Json::objectValue);
	add_id(event, id);
	event["event"] = "error";
	event["code"] = code;
	event["message"] = message;
	std::string line = JsonString(event);
	fprintf(out, "%s\n", line.c_str());
}

void Fox_Protocol::WriteLogEvent(FILE* out, const std::string& id, const char* data, size_t size)
{
	if (!out || !data || size == 0)
		return;
	Json::Value event(Json::objectValue);
	add_id(event, id);
	event["event"] = "log";
	event["text"] = std::string(data, size);
	std::string line = JsonString(event);
	fprintf(out, "%s\n", line.c_str());
}

void Fox_Protocol::WriteProgressEvent(FILE* out, const std::string& id, const std::string& phase, int percent)
{
	if (!out)
		return;
	if (percent < 0)
		percent = 0;
	if (percent > 100)
		percent = 100;
	Json::Value event(Json::objectValue);
	add_id(event, id);
	event["event"] = "progress";
	event["phase"] = phase;
	event["percent"] = percent;
	std::string line = JsonString(event);
	fprintf(out, "%s\n", line.c_str());
}

void Fox_Protocol::WriteDataEvent(FILE* out, const std::string& id, const std::string& name, const std::string& json)
{
	if (!out)
		return;
	// Parse the serialized payload back into a value, falling back to a raw
	// string value when it is not valid JSON, then emit via the value overload.
	Json::Value value;
	Json::CharReaderBuilder builder;
	builder["collectComments"] = false;
	std::string errors;
	std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
	if (!reader->parse(json.data(), json.data() + json.size(), &value, &errors))
		value = json;

	WriteDataEvent(out, id, name, value);
}

void Fox_Protocol::WriteDataEvent(FILE* out, const std::string& id, const std::string& name, const Json::Value& value)
{
	if (!out)
		return;
	Json::Value event(Json::objectValue);
	add_id(event, id);
	event["event"] = "data";
	event["name"] = name;
	event["value"] = value;
	std::string line = JsonString(event);
	fprintf(out, "%s\n", line.c_str());
}

FILE* Fox_Protocol::OpenEventLogFile(FILE* out, const std::string& id)
{
	if (!out)
		return nullptr;
	EventLogCookie* cookie = new EventLogCookie();
	cookie->out = out;
	cookie->id = id;
	FILE* file = funopen(cookie, /*readfn=*/nullptr, event_write,
	                     /*seekfn=*/nullptr, event_close);
	if (!file) {
		delete cookie;
		return nullptr;
	}
	setvbuf(file, nullptr, _IONBF, 0);
	return file;
}

void Fox_Protocol::WriteKvBlock(FILE* out, const char* begin, const std::map<std::string, std::string>& kv, const char* end)
{
	if (!out)
		return;
	fprintf(out, "%s\n", begin);
	for (std::map<std::string, std::string>::const_iterator it = kv.begin(); it != kv.end(); ++it)
		fprintf(out, "%s=%s\n", it->first.c_str(), it->second.c_str());
	fprintf(out, "%s\n", end);
}

int Fox_Protocol::ClampFps(int fps)
{
	if (fps < 1)
		return 1;
	if (fps > 30)
		return 30;
	return fps;
}

std::string Fox_Protocol::JsonEscape(const std::string& in)
{
	std::string out;
	for (char c : in) {
		switch (c) {
			case '"':  out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n";  break;
			case '\r': out += "\\r";  break;
			case '\t': out += "\\t";  break;
			default:
				if ((unsigned char)c < 0x20) {
					char b[8];
					snprintf(b, sizeof(b), "\\u%04x", (unsigned char)c);
					out += b;
				} else {
					out += c;
				}
		}
	}
	return out;
}

std::string Fox_Protocol::JsonString(const Json::Value& value)
{
	return write_json(value);
}
