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

#ifndef _FOX_SCREEN_HPP
#define _FOX_SCREEN_HPP

#include <cstdint>
#include <string>

// Fox_Screen grabs the live recovery framebuffer as PNG frames for the remote
// viewer.
//
// Correctness note (this is subtle): gr_save_screenshot() reads gr_mem_surface,
// whose .data pointer gr_flip() repoints to the *back* buffer after every flip.
// OrangeFox also only renders when the screen changes. So capturing from the
// HTTP thread asynchronously (a) reads the off-screen buffer and (b) races
// gr_flip() rewriting that pointer mid-read -> split / half-stale frames.
//
// Therefore frames are captured on the GUI thread, immediately after
// PageManager::Render() and before flip(), when gr_mem_surface holds the
// complete just-rendered frame and nothing else touches it. The HTTP thread
// only ever reads the finished PNG snapshot produced there. This also makes the
// stream event-driven: a new frame is produced exactly when the screen changes.
class Fox_Screen
{
public:
	// Framebuffer dimensions (0 before graphics init).
	static int Width();
	static int Height();

	// ---- GUI-thread side --------------------------------------------------

	// Capture the freshly rendered frame into the shared snapshot, if a viewer
	// is active and the minimum inter-frame interval has elapsed. Cheap no-op
	// otherwise. MUST be called on the GUI thread, after PageManager::Render()
	// and before flip().
	static void CaptureFrameIfWanted();

	// Capture the freshly rendered frame immediately, regardless of viewer
	// state or rate limit. MUST be called on the GUI thread, after
	// PageManager::Render() and before flip().
	static bool CaptureFrameNow(std::string& png);

	// True when a viewer wants frames but the screen is idle, so the main loop
	// should force a full render to produce a fresh frame. GUI thread only.
	static bool ShouldForceRender();

	// ---- HTTP-thread side -------------------------------------------------

	// Register interest in frames; keeps capture alive for a few seconds.
	static void RequestFrames();

	// Copy the latest captured PNG. Returns false if none has been produced.
	// When generation is non-null, it receives a monotonically increasing
	// snapshot id for freshness checks.
	static bool GetLatest(std::string& png, uint64_t* generation = nullptr);
};

#endif // _FOX_SCREEN_HPP
