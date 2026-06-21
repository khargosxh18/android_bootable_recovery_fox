/*
	Copyright 2017 TeamWin
	This file is part of TWRP/TeamWin Recovery Project.

	TWRP is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	TWRP is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with TWRP.  If not, see <http://www.gnu.org/licenses/>.
*/

// fill.cpp - GUIFill object

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>

#include <string>

extern "C" {
#include "../twcommon.h"
}
#include "minuitwrp/minui.h"

#include "rapidxml.hpp"
#include "objects.hpp"

GUIFill::GUIFill(xml_node<>* node) : GUIObject(node)
{
	bool has_color = false;
	mCircle = NULL;
	mColor = LoadAttrColor(node, "color", &has_color);
	if (!has_color) {
		LOGERR("No color specified for fill\n");
		return;
	}

	// Load the placement
	LoadPlacement(FindNode(node, "placement"), &mRenderX, &mRenderY, &mRenderW, &mRenderH);

	return;
}

GUIFill::~GUIFill()
{
	if (mCircle)
		gr_free_surface(mCircle);
}

GUIDivider::GUIDivider(xml_node<>* node) : GUIObject(node)
{
	// Everything except <spacer> paints a filled rect (<divider>, and the
	// background of a <card>).
	std::string nm = node ? node->name() : "";
	mPaint = (nm != "spacer");
	bool isDivider = (nm == "divider");

	// Position/size straight off the element's attributes (a layout container
	// overrides x/y). A divider defaults to a full-width 2px rule.
	mRenderX = LoadAttrIntScaleX(node, "x", 0);
	mRenderY = LoadAttrIntScaleY(node, "y", 0);
	mRenderW = LoadAttrIntScaleX(node, "w", 0);
	mRenderH = LoadAttrIntScaleY(node, "h", isDivider ? 2 : 0);
	if (isDivider && mRenderW == 0)
		mRenderW = gr_fb_width();

	// Default to a subtle neutral rule; override with color=.
	COLOR def;
	def.red = def.green = def.blue = 128;
	def.alpha = 60;
	mColor = LoadAttrColor(node, "color", def);
}

int GUIDivider::Render(void)
{
	if (!isConditionTrue() || !mPaint)
		return 0;

	gr_color(mColor.red, mColor.green, mColor.blue, mColor.alpha);
	gr_fill(mRenderX, mRenderY, mRenderW, mRenderH);
	return 0;
}

int GUIFill::Render(void)
{
	if (!isConditionTrue())
		return 0;

	// A fully-transparent fill is invisible yet still costs a full-region alpha
	// blend; skip it (e.g. <fill color="transparent"> used as a touch backing).
	if (mColor.alpha == 0)
		return 0;

	gr_color(mColor.red, mColor.green, mColor.blue, mColor.alpha);
	gr_fill(mRenderX, mRenderY, mRenderW, mRenderH);

	return 0;
}

