/*
	Copyright (C) 2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.

	OrangeFox is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#include "fox_screen_stream.hpp"

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <string>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "fox_protocol.hpp"
#include "fox_command_dispatcher.hpp"
#include "fox_screen_service.hpp"
#include "../orscmd/orscmd.h"
#include "../twcommon.h"

namespace {

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
pthread_t g_thread;
bool g_thread_running = false;
bool g_active = false;
bool g_have_frame = false;
std::string g_frame;
int g_fps = 10;
long long g_last_capture_ms = 0;

long long now_ms()
{
	struct timeval t;
	gettimeofday(&t, nullptr);
	return (long long)t.tv_sec * 1000LL + t.tv_usec / 1000;
}

bool capture_due()
{
	pthread_mutex_lock(&g_lock);
	bool active = g_active;
	int fps = g_fps;
	long long last = g_last_capture_ms;
	pthread_mutex_unlock(&g_lock);
	if (!active)
		return false;
	if (fps < 1)
		fps = 1;
	return now_ms() - last >= 1000 / fps;
}

void submit_frame(const std::string& png)
{
	pthread_mutex_lock(&g_lock);
	if (g_active) {
		g_frame = png;
		g_have_frame = true;
		g_last_capture_ms = now_ms();
		pthread_cond_signal(&g_cond);
	}
	pthread_mutex_unlock(&g_lock);
}

bool write_all(int fd, const std::string& data)
{
	size_t off = 0;
	while (off < data.size()) {
		pthread_mutex_lock(&g_lock);
		bool active = g_active;
		pthread_mutex_unlock(&g_lock);
		if (!active)
			return false;
		ssize_t n = write(fd, data.data() + off, data.size() - off);
		if (n > 0) {
			off += (size_t)n;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			usleep(10 * 1000);
			continue;
		}
		return false;
	}
	return true;
}

void* writer_thread(void*)
{
	signal(SIGPIPE, SIG_IGN);
	int fd = -1;
	for (;;) {
		pthread_mutex_lock(&g_lock);
		while (g_active && !g_have_frame)
			pthread_cond_wait(&g_cond, &g_lock);
		if (!g_active) {
			g_thread_running = false;
			pthread_mutex_unlock(&g_lock);
			if (fd >= 0)
				close(fd);
			return nullptr;
		}
		std::string png = g_frame;
		g_have_frame = false;
		pthread_mutex_unlock(&g_lock);

		if (fd < 0) {
			fd = open(FOX_SCREEN_STREAM_FILE, O_WRONLY | O_NONBLOCK);
			if (fd < 0) {
				usleep(100 * 1000);
				continue;
			}
#ifdef F_SETPIPE_SZ
			fcntl(fd, F_SETPIPE_SZ, 1024 * 1024);
#endif
		}

		std::string b64 = Fox_Protocol::Base64Encode(png);
		if (!write_all(fd, b64)) {
			close(fd);
			fd = -1;
		}
	}
}

} // namespace

bool Fox_Screen_Stream::IsActive()
{
	pthread_mutex_lock(&g_lock);
	bool active = g_active;
	pthread_mutex_unlock(&g_lock);
	return active;
}

bool Fox_Screen_Stream::ShouldForceRender()
{
	return capture_due();
}

bool Fox_Screen_Stream::CaptureRenderedFrameIfDue()
{
	if (!capture_due())
		return false;
	std::string png;
	if (!Fox_Screen_Service::CaptureRenderedFrameNow(png))
		return false;
	submit_frame(png);
	return true;
}

int Fox_Screen_Stream::Start(int fps)
{
	fps = Fox_Protocol::ClampFps(fps);

	pthread_mutex_lock(&g_lock);
	if (g_active) {
		g_fps = fps;
		pthread_mutex_unlock(&g_lock);
		return 0;
	}
	pthread_mutex_unlock(&g_lock);

	unlink(FOX_SCREEN_STREAM_FILE);
	if (mkfifo(FOX_SCREEN_STREAM_FILE, 06666) != 0) {
		LOGINFO("Unable to mkfifo %s\n", FOX_SCREEN_STREAM_FILE);
		return 1;
	}

	pthread_mutex_lock(&g_lock);
	g_fps = fps;
	g_active = true;
	g_have_frame = false;
	g_frame.clear();
	g_last_capture_ms = 0;
	bool need_thread = !g_thread_running;
	if (need_thread)
		g_thread_running = true;
	pthread_cond_signal(&g_cond);
	pthread_mutex_unlock(&g_lock);

	if (need_thread && pthread_create(&g_thread, nullptr, writer_thread, nullptr) != 0) {
		pthread_mutex_lock(&g_lock);
		g_active = false;
		g_thread_running = false;
		pthread_mutex_unlock(&g_lock);
		unlink(FOX_SCREEN_STREAM_FILE);
		return 1;
	}
	if (need_thread)
		pthread_detach(g_thread);
	return 0;
}

void Fox_Screen_Stream::Stop()
{
	pthread_mutex_lock(&g_lock);
	g_active = false;
	g_have_frame = false;
	g_frame.clear();
	pthread_cond_signal(&g_cond);
	pthread_mutex_unlock(&g_lock);

	// If a client is blocked opening/reading the stream FIFO, briefly pair it
	// with a writer so it sees EOF and the dashboard's cat process can exit.
	int fd = open(FOX_SCREEN_STREAM_FILE, O_WRONLY | O_NONBLOCK);
	if (fd >= 0)
		close(fd);
}

int Fox_Screen_Stream::HandleCommand(const Json::Value& args, FILE* out)
{
	std::string sub = args.isObject() && args["action"].isString() ? args["action"].asString() : "";
	if (sub.empty())
		sub = "status";
	if (sub == "start") {
		int fps = 10;
		const Json::Value& v = args["fps"];
		if (v.isIntegral())
			fps = (int)v.asInt64();
		else if (v.isString() && !v.asString().empty())
			fps = atoi(v.asString().c_str());
		int rc = Start(fps);
		if (rc == 0) {
			Json::Value s(Json::objectValue);
			s["running"] = true;
			s["path"] = FOX_SCREEN_STREAM_FILE;
			s["fps"] = Fox_Protocol::ClampFps(fps);
			s["format"] = "base64-png-lines";
			Fox_Command_Dispatcher::EmitData("screenstream", s);
		} else {
			fprintf(out, "fox: unable to start screen stream\n");
		}
		return rc;
	}
	if (sub == "stop") {
		Stop();
		fprintf(out, "Screen stream stopped\n");
		return 0;
	}
	if (sub == "status") {
		pthread_mutex_lock(&g_lock);
		int fps = g_fps;
		pthread_mutex_unlock(&g_lock);
		Json::Value s(Json::objectValue);
		s["running"] = IsActive();
		s["path"] = FOX_SCREEN_STREAM_FILE;
		s["fps"] = fps;
		Fox_Command_Dispatcher::EmitData("screenstream", s);
		return 0;
	}

	fprintf(out, "fox: unknown screenstream subcommand '%s' (start|stop|status)\n", sub.c_str());
	return 2;
}
