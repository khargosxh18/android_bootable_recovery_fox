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

#include <cstring>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "fox_input.hpp"
#include "../twcommon.h"
#include "minuitwrp/minui.h"

namespace {

int g_fd = -1;
int g_tracking_id = 0;
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

void emit(int type, int code, int value) {
	if (g_fd < 0)
		return;
	struct input_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = type;
	ev.code = code;
	ev.value = value;
	// Best-effort: a short write to a uinput node should not happen, and there
	// is nothing useful to do if the kernel buffer is momentarily full.
	if (write(g_fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) {
		// ignore
	}
}

void sync_report() {
	emit(EV_SYN, SYN_REPORT, 0);
}

}  // namespace

bool Fox_Input::Init() {
	pthread_mutex_lock(&g_lock);
	if (g_fd >= 0) {
		pthread_mutex_unlock(&g_lock);
		return true;
	}

	int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (fd < 0)
		fd = open("/dev/input/uinput", O_WRONLY | O_NONBLOCK);
	if (fd < 0) {
		LOGERR("Fox_Input: cannot open /dev/uinput\n");
		pthread_mutex_unlock(&g_lock);
		return false;
	}

	int w = gr_fb_width();
	int h = gr_fb_height();
	if (w <= 0) w = 1080;
	if (h <= 0) h = 1920;

	// Event types we will emit.
	ioctl(fd, UI_SET_EVBIT, EV_SYN);
	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	ioctl(fd, UI_SET_EVBIT, EV_ABS);

	// Touch button + a broad keyboard key set so any KEY_* can be injected.
	ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);
	for (int code = 1; code <= KEY_MAX && code < 0x2ff; code++)
		ioctl(fd, UI_SET_KEYBIT, code);

	// Multitouch (type-B) absolute axes.
	ioctl(fd, UI_SET_ABSBIT, ABS_MT_SLOT);
	ioctl(fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
	ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_X);
	ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_Y);

	// Legacy device setup (struct uinput_user_dev) -- compatible with all
	// kernels, unlike UI_DEV_SETUP/UI_ABS_SETUP which need >= 4.5.
	struct uinput_user_dev uidev;
	memset(&uidev, 0, sizeof(uidev));
	snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "OrangeFox Virtual Touch");
	uidev.id.bustype = BUS_VIRTUAL;
	uidev.id.vendor = 0x0F0F;
	uidev.id.product = 0x0001;
	uidev.id.version = 1;

	uidev.absmin[ABS_MT_POSITION_X] = 0;
	uidev.absmax[ABS_MT_POSITION_X] = w - 1;
	uidev.absmin[ABS_MT_POSITION_Y] = 0;
	uidev.absmax[ABS_MT_POSITION_Y] = h - 1;
	uidev.absmin[ABS_MT_SLOT] = 0;
	uidev.absmax[ABS_MT_SLOT] = 9;
	uidev.absmin[ABS_MT_TRACKING_ID] = 0;
	uidev.absmax[ABS_MT_TRACKING_ID] = 65535;

	if (write(fd, &uidev, sizeof(uidev)) != (ssize_t)sizeof(uidev)) {
		LOGERR("Fox_Input: failed to write uinput_user_dev\n");
		close(fd);
		pthread_mutex_unlock(&g_lock);
		return false;
	}
	if (ioctl(fd, UI_DEV_CREATE) != 0) {
		LOGERR("Fox_Input: UI_DEV_CREATE failed\n");
		close(fd);
		pthread_mutex_unlock(&g_lock);
		return false;
	}

	g_fd = fd;
	pthread_mutex_unlock(&g_lock);

	// Give udev/devtmpfs a moment so the new /dev/input/eventN node appears and
	// the GUI's ev_get() re-scan picks it up (it polls /dev/input mtime ~2s).
	LOGINFO("Fox_Input: virtual touch/keyboard device created (%dx%d)\n", w, h);
	return true;
}

void Fox_Input::Shutdown() {
	pthread_mutex_lock(&g_lock);
	if (g_fd >= 0) {
		ioctl(g_fd, UI_DEV_DESTROY);
		close(g_fd);
		g_fd = -1;
	}
	pthread_mutex_unlock(&g_lock);
}

bool Fox_Input::IsReady() {
	pthread_mutex_lock(&g_lock);
	bool ready = g_fd >= 0;
	pthread_mutex_unlock(&g_lock);
	return ready;
}

void Fox_Input::TouchDown(int x, int y) {
	pthread_mutex_lock(&g_lock);
	emit(EV_ABS, ABS_MT_SLOT, 0);
	emit(EV_ABS, ABS_MT_TRACKING_ID, g_tracking_id++ & 0xFFFF);
	emit(EV_KEY, BTN_TOUCH, 1);
	emit(EV_ABS, ABS_MT_POSITION_X, x);
	emit(EV_ABS, ABS_MT_POSITION_Y, y);
	sync_report();
	pthread_mutex_unlock(&g_lock);
}

void Fox_Input::TouchMove(int x, int y) {
	pthread_mutex_lock(&g_lock);
	emit(EV_ABS, ABS_MT_SLOT, 0);
	emit(EV_ABS, ABS_MT_POSITION_X, x);
	emit(EV_ABS, ABS_MT_POSITION_Y, y);
	sync_report();
	pthread_mutex_unlock(&g_lock);
}

void Fox_Input::TouchUp() {
	pthread_mutex_lock(&g_lock);
	emit(EV_ABS, ABS_MT_SLOT, 0);
	emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
	emit(EV_KEY, BTN_TOUCH, 0);
	sync_report();
	pthread_mutex_unlock(&g_lock);
}

void Fox_Input::Tap(int x, int y) {
	TouchDown(x, y);
	struct timespec ts = { 0, 40 * 1000 * 1000 };  // 40 ms
	nanosleep(&ts, nullptr);
	TouchUp();
}

void Fox_Input::Key(int code, bool down) {
	pthread_mutex_lock(&g_lock);
	emit(EV_KEY, code, down ? 1 : 0);
	sync_report();
	pthread_mutex_unlock(&g_lock);
}

void Fox_Input::KeyTap(int code) {
	Key(code, true);
	struct timespec ts = { 0, 20 * 1000 * 1000 };  // 20 ms
	nanosleep(&ts, nullptr);
	Key(code, false);
}
