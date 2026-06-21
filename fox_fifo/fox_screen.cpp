/*
	Copyright (C) 2024-2026 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.

	OrangeFox is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	OrangeFox is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <cstdio>
#include <pthread.h>
#include <time.h>

#include "fox_screen.hpp"

#include "minuitwrp/minui.h"

namespace {

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
std::string g_latest;        // most recent encoded PNG (complete frame)
uint64_t g_generation = 0;   // increments after each complete captured frame
long g_wanted_until_ms = 0;  // viewers want frames until this time
long g_last_capture_ms = 0;  // when we last captured
bool g_pending = false;      // a render happened but capture was rate-limited

// Per-frame capture floor -> ~16 fps ceiling. Each capture is a full GGL
// render-to-RGBA + PNG encode on the GUI thread, so we cap it to avoid stealing
// too much time from the UI on animated screens. This is small so that a
// discrete change (e.g. a page switch) is captured promptly, not skipped.
const long kMinIntervalMs = 60;
// Idle refresh: when the screen is static, force a fresh frame only this often
// (safety net / first frame for a new viewer). Kept large so we don't burn the
// CPU force-rendering, and so the capture floor above rarely eats a change.
const long kIdleForceMs = 1000;
// How long a single RequestFrames() keeps capture alive.
const long kWantedWindowMs = 4000;

long now_ms() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Reuse the already-exported gr_save_screenshot() (writes a PNG to a path) via a
// tmpfs scratch file -- avoids adding a new exported symbol to libminuitwrp.
// Caller must be on the GUI thread, between Render() and flip().
bool capture_png(std::string& png) {
	const char* path = "/tmp/.fox_screen.png";
	if (gr_save_screenshot(path) != 0)
		return false;
	FILE* f = fopen(path, "rb");
	if (!f)
		return false;
	bool ok = false;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz > 0) {
		png.resize((size_t)sz);
		ok = fread(&png[0], 1, (size_t)sz, f) == (size_t)sz;
	}
	fclose(f);
	return ok;
}

}  // namespace

void Fox_Screen::CaptureFrameIfWanted() {
	// Called on the GUI thread after every Render() (i.e. the screen just
	// changed). Capture this frame unless we're under the per-frame floor, in
	// which case remember it as pending so ShouldForceRender() flushes it ASAP
	// rather than letting it wait for the slow idle refresh.
	long now = now_ms();
	pthread_mutex_lock(&g_lock);
	bool wanted = now < g_wanted_until_ms;
	bool due = (now - g_last_capture_ms) >= kMinIntervalMs;
	if (wanted && !due)
		g_pending = true;
	pthread_mutex_unlock(&g_lock);
	if (!wanted || !due)
		return;

	std::string png;
	if (!capture_png(png))
		return;

	pthread_mutex_lock(&g_lock);
	g_latest.swap(png);
	++g_generation;
	g_last_capture_ms = now;
	g_pending = false;
	pthread_mutex_unlock(&g_lock);
}

bool Fox_Screen::CaptureFrameNow(std::string& png) {
	if (!capture_png(png))
		return false;

	long now = now_ms();
	pthread_mutex_lock(&g_lock);
	g_latest = png;
	++g_generation;
	g_last_capture_ms = now;
	g_pending = false;
	pthread_mutex_unlock(&g_lock);
	return true;
}

bool Fox_Screen::ShouldForceRender() {
	long now = now_ms();
	pthread_mutex_lock(&g_lock);
	bool wanted = now < g_wanted_until_ms;
	long since = now - g_last_capture_ms;
	// If a change was skipped by the floor, flush it as soon as the floor
	// passes; otherwise just refresh slowly so a new viewer / missed change is
	// eventually picked up on a static screen.
	bool force = wanted && (g_pending ? (since >= kMinIntervalMs)
	                                  : (since >= kIdleForceMs));
	pthread_mutex_unlock(&g_lock);
	return force;
}

void Fox_Screen::RequestFrames() {
	long now = now_ms();
	pthread_mutex_lock(&g_lock);
	g_wanted_until_ms = now + kWantedWindowMs;
	pthread_mutex_unlock(&g_lock);
}

bool Fox_Screen::GetLatest(std::string& png, uint64_t* generation) {
	pthread_mutex_lock(&g_lock);
	bool ok = !g_latest.empty();
	if (ok) {
		png = g_latest;
		if (generation)
			*generation = g_generation;
	}
	pthread_mutex_unlock(&g_lock);
	return ok;
}

int Fox_Screen::Width() {
	return gr_fb_width();
}

int Fox_Screen::Height() {
	return gr_fb_height();
}
