/*
	Copyright 2024 OrangeFox Recovery Project
	This file is part of TWRP/TeamWin Recovery Project.

	TWRP is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	TWRP is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.
*/

// toggle.cpp - FoxUiEngine GUIToggle: a labelled on/off switch row bound to a
// DataManager variable. See objects.hpp for the markup.

#include <stdlib.h>
#include <string>

extern "C" {
#include "../twcommon.h"
}
#include "minuitwrp/minui.h"

#include "rapidxml.hpp"
#include "objects.hpp"
#include "pages.hpp"
#include "../data.hpp"

GUIToggle::GUIToggle(xml_node<>* node) : GUIObject(node)
{
	mLabel = NULL;
	mAction = NULL;
	mPressed = false;

	// Defaults: ON = theme accent, OFF = neutral grey, knob = white.
	mTrackOff.red = mTrackOff.green = mTrackOff.blue = 150; mTrackOff.alpha = 255;
	mKnob.red = mKnob.green = mKnob.blue = mKnob.alpha = 255;
	if (ConvertStrToColor(DataManager::GetStrValue("accent"), &mTrackOn) != 0) {
		mTrackOn.red = 80; mTrackOn.green = 160; mTrackOn.blue = 230; mTrackOn.alpha = 255;
	}

	if (!node)
		return;

	mVar = LoadAttrString(node, "var");

	LoadPlacement(FindNode(node, "placement"), &mRenderX, &mRenderY, &mRenderW, &mRenderH);
	SetActionPos(mRenderX, mRenderY, mRenderW, mRenderH);

	// Optional colour overrides.
	mTrackOn = LoadAttrColor(node, "oncolor", mTrackOn);
	mTrackOff = LoadAttrColor(node, "offcolor", mTrackOff);
	mKnob = LoadAttrColor(node, "knobcolor", mKnob);

	// The label is just a GUIText built from this node (renders <text>/<font>
	// at the node's placement, i.e. the left of the row).
	mLabel = new GUIText(node);
	if (mLabel->Render() < 0) {
		delete mLabel;
		mLabel = NULL;
	}

	// Child <action>s (optional side effects) fire on toggle.
	mAction = new GUIAction(node);
}

GUIToggle::~GUIToggle()
{
	delete mLabel;
	delete mAction;
}

int GUIToggle::Render(void)
{
	if (!isConditionTrue())
		return 0;

	if (mLabel)
		mLabel->Render();

	int state = 0;
	DataManager::GetValue(mVar, state);

	// Pill geometry: right-aligned within the row, vertically centred.
	int pillH = mRenderH / 2;
	if (pillH < 8)
		pillH = 8;
	int pillW = pillH * 9 / 5; // ~1.8 aspect
	int px = mRenderX + mRenderW - pillW;
	int py = mRenderY + (mRenderH - pillH) / 2;

	const COLOR& track = state ? mTrackOn : mTrackOff;
	gr_color(track.red, track.green, track.blue, track.alpha);
	gr_fill(px, py, pillW, pillH);

	int pad = pillH / 6;
	int knob = pillH - 2 * pad;
	int kx = state ? (px + pillW - knob - pad) : (px + pad);
	gr_color(mKnob.red, mKnob.green, mKnob.blue, mKnob.alpha);
	gr_fill(kx, py + pad, knob, knob);

	return 0;
}

int GUIToggle::NotifyTouch(TOUCH_STATE state, int x, int y)
{
	// Only flip when the press both started and ended inside the toggle, so a
	// drag that merely happens to release here (or a press elsewhere) won't trip
	// it. Track the press with mPressed.
	if (state == TOUCH_START)
		mPressed = IsInRegion(x, y);
	else if (state == TOUCH_RELEASE) {
		if (mPressed && IsInRegion(x, y) && !mVar.empty()) {
			int v = 0;
			DataManager::GetValue(mVar, v);
			DataManager::SetValue(mVar, v ? 0 : 1);
			gui_forceRender();
		}
		mPressed = false;
	}
	// Let any child <action>s fire (side effects read the new value).
	return mAction ? mAction->NotifyTouch(state, x, y) : 0;
}

int GUIToggle::NotifyVarChange(const std::string& varName, const std::string& value)
{
	GUIObject::NotifyVarChange(varName, value);
	if (mLabel)
		mLabel->NotifyVarChange(varName, value);
	if (!mVar.empty() && varName == mVar)
		gui_forceRender();
	return 0;
}
