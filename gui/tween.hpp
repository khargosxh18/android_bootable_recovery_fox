/*
	Copyright 2024 OrangeFox Recovery Project
	This file is part of TWRP/TeamWin Recovery Project. GPLv3+.
*/

// tween.hpp - FoxUiEngine: a tiny frame-based property tween for intro/exit
// animations. The GUI ticks Update() ~30x/sec; a Tween counts a frame budget
// down to zero and reports an eased offset that any RenderObject can add to a
// position (slide) so elements animate into place. Header-only, no deps.

#ifndef _FOXUI_TWEEN_HPP
#define _FOXUI_TWEEN_HPP

namespace FoxUiEngine {

struct Tween
{
	int frames; // remaining frames (0 = finished)
	int total;  // total frames in the animation
	int amount; // start offset (eased to 0 as frames -> 0)

	Tween() : frames(0), total(1), amount(0) {}

	// Begin a `totalFrames`-long animation sliding from `amt` px to 0.
	void start(int totalFrames, int amt)
	{
		total = totalFrames > 0 ? totalFrames : 1;
		frames = total;
		amount = amt;
	}

	bool active() const { return frames > 0; }

	// Advance one frame. Call once per Update().
	void step() { if (frames > 0) frames--; }

	// Eased current offset (ease-out quad: amount*(frames/total)^2 -> 0).
	int value() const
	{
		long f = frames;
		return (int)((long)amount * f * f / ((long)total * total));
	}
};

} // namespace FoxUiEngine

#endif // _FOXUI_TWEEN_HPP
