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

#ifndef _FOX_INPUT_HPP
#define _FOX_INPUT_HPP

// Fox_Input injects synthetic touch and key events for remote control via a
// /dev/uinput virtual device. Because minuitwrp's ev_get() re-scans /dev/input
// when its mtime changes, the virtual device is picked up by the normal GUI
// input pipeline automatically -- no changes to the input handling are needed.
//
// The virtual touchscreen advertises an ABS range of [0, fb_width/height) so
// minuitwrp's calibration maps injected coordinates 1:1 to screen pixels.
//
// All methods are safe to call from the HTTP server thread; writes to the
// uinput fd are atomic per event and serialised by an internal lock.
class Fox_Input
{
public:
	// Create the virtual device, sized to the current framebuffer. Safe to call
	// repeatedly (no-op if already initialised). Returns false if /dev/uinput is
	// unavailable or device creation fails.
	static bool Init();

	// Tear down the virtual device.
	static void Shutdown();

	static bool IsReady();

	// ---- Touch (single finger, multitouch type-B protocol) ----------------
	static void TouchDown(int x, int y);
	static void TouchMove(int x, int y);
	static void TouchUp();
	// Convenience: a full down/up tap at (x, y).
	static void Tap(int x, int y);

	// ---- Keys -------------------------------------------------------------
	// 'code' is a Linux input KEY_* code. value: true = press, false = release.
	static void Key(int code, bool down);
	// Convenience: press + release.
	static void KeyTap(int code);
};

#endif // _FOX_INPUT_HPP
