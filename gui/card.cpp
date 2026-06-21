/*
	Copyright 2024 OrangeFox Recovery Project
	This file is part of TWRP/TeamWin Recovery Project. GPLv3+.
*/

// card.cpp - FoxUiEngine GUICard: an owning, content-sizing rounded container.
// See objects.hpp for the markup and the design notes.

#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

extern "C" {
#include "../twcommon.h"
}
#include "minuitwrp/minui.h"

#include "rapidxml.hpp"
#include "objects.hpp"
#include "../data.hpp"

// Build a persistent (rounded) filled rect (stroke==0) or outline (stroke>0)
// surface. The caller owns it and must free it with free_shape_surface(); we
// keep these around across frames so the animation doesn't re-rasterise the
// rounded rect every frame (that software fill is what made the slide lag).
static gr_surface make_shape_surface(int w, int h, int radius, int stroke, COLOR color)
{
	if (w <= 0 || h <= 0)
		return NULL;
	uint32_t* img = createShape(w, h, radius, stroke, color);
	if (!img)
		return NULL;

	GGLSurface* surface = (GGLSurface*)malloc(sizeof(GGLSurface));
	if (!surface) {
		free(img);
		return NULL;
	}
	memset(surface, 0, sizeof(GGLSurface));
	surface->version = sizeof(surface);
	surface->width = w;
	surface->height = h;
	surface->stride = w;
	surface->data = (GGLubyte*)img;
	surface->format = res_get_pixel_format();
	return (gr_surface)surface;
}

static void free_shape_surface(gr_surface s)
{
	if (!s)
		return;
	GGLSurface* g = (GGLSurface*)s;
	void* data = g->data;
	res_free_surface(s);
	free(data);
}

static bool color_eq(const COLOR& a, const COLOR& b)
{
	return a.red == b.red && a.green == b.green && a.blue == b.blue && a.alpha == b.alpha;
}

GUICard::GUICard(xml_node<>* node) : GUIObject(node)
{
	mTouchChild = NULL;
	mHorizontal = false;
	mSpacing = mPadding = mAlign = 0;
	mMaxWidth = 0;
	mFixedWidth = 0;
	mCenterX = false;
	mBottom = -1;
	mPosX = mPosY = 0;
	mRadius = 0;
	mStroke = 4;
	mHasOutline = false;
	mIntroFrames = 0;
	mIntroAmount = 0;
	mBgSurface = mOutlineSurface = NULL;
	mCacheW = mCacheH = mCacheRadius = mCacheStroke = -1;
	memset(&mCacheBg, 0, sizeof(mCacheBg));
	memset(&mCacheOutline, 0, sizeof(mCacheOutline));
	mRenderX = mRenderY = mRenderW = mRenderH = 0;

	mBgColor.red = mBgColor.green = mBgColor.blue = 32;
	mBgColor.alpha = 255;
	mOutlineColor.red = mOutlineColor.green = mOutlineColor.blue = mOutlineColor.alpha = 0;

	if (!node)
		return;

	mHorizontal = (LoadAttrString(node, "direction") == "horizontal");

	std::string al = LoadAttrString(node, "align");
	if (al == "center")
		mAlign = 1;
	else if (al == "end")
		mAlign = 2;

	mPadding = LoadAttrIntScaleX(node, "padding", 0);
	mSpacing = LoadAttrIntScaleX(node, "spacing", 0);
	mRadius = LoadAttrIntScaleX(node, "radius", 0);
	mStroke = LoadAttrIntScaleX(node, "outlinewidth", 4);
	mMaxWidth = LoadAttrIntScaleX(node, "maxwidth", 0);
	mFixedWidth = LoadAttrIntScaleX(node, "width", 0);
	mCenterX = (LoadAttrInt(node, "centerx", 0) == 1);

	// intro="<px>" gives a subtle slide-up-and-settle when the card appears.
	// intro="1" picks a sensible default offset; intro="<n>" sets the px amount.
	if (node->first_attribute("intro")) {
		int amt = LoadAttrIntScaleY(node, "intro", 0);
		mIntroAmount = (amt <= 1) ? 40 : amt;
		mIntroFrames = LoadAttrInt(node, "introframes", 6);
	}

	if (node->first_attribute("bottom"))
		mBottom = LoadAttrIntScaleY(node, "bottom", -1);
	if (node->first_attribute("x"))
		mPosX = LoadAttrIntScaleX(node, "x", 0);
	if (node->first_attribute("y"))
		mPosY = LoadAttrIntScaleY(node, "y", 0);

	mBgColor = LoadAttrColor(node, "color", mBgColor);
	if (node->first_attribute("outline")) {
		mHasOutline = true;
		mOutlineColor = LoadAttrColor(node, "outline", mOutlineColor);
	}
	// Dynamic outline: name a DataManager var holding a colour string (e.g. the
	// toast's per-severity colour). Re-read each frame so it tracks the var.
	mOutlineVar = LoadAttrString(node, "outlinevar", "");
	if (!mOutlineVar.empty())
		mHasOutline = true;
}

GUICard::~GUICard()
{
	free_shape_surface(mBgSurface);
	free_shape_surface(mOutlineSurface);
}

void GUICard::Adopt(GUIObject* obj, RenderObject* ro, ActionObject* ao)
{
	Child c;
	c.obj = obj;
	c.render = ro;
	c.action = ao;
	mChildren.push_back(c);
}

void GUICard::Layout(void)
{
	size_t n = mChildren.size();
	std::vector<int> mainSize(n, 0), crossSize(n, 0);
	std::vector<bool> vis(n, false);

	// Pass 1 - measure (text is measured multi-line aware via GetCurrentBounds).
	int contentMain = 0, maxCross = 0, visCount = 0;
	for (size_t i = 0; i < n; i++) {
		Child& c = mChildren[i];
		if (!c.render || (c.obj && !c.obj->isConditionTrue()))
			continue;
		vis[i] = true;

		int cw = 0, ch = 0;
		GUIText* t = dynamic_cast<GUIText*>(c.obj);
		if (t) {
			t->GetCurrentBounds(cw, ch);
		} else {
			int cx = 0, cy = 0;
			c.render->GetRenderPos(cx, cy, cw, ch);
		}
		mainSize[i] = mHorizontal ? cw : ch;
		crossSize[i] = mHorizontal ? ch : cw;

		if (visCount > 0)
			contentMain += mSpacing;
		contentMain += mainSize[i];
		if (crossSize[i] > maxCross)
			maxCross = crossSize[i];
		visCount++;
	}

	// Card size = content + padding (clamped to maxwidth on the width axis).
	int W = (mHorizontal ? contentMain : maxCross) + 2 * mPadding;
	int H = (mHorizontal ? maxCross : contentMain) + 2 * mPadding;
	if (mFixedWidth > 0)
		W = mFixedWidth;           // span a fixed width (e.g. full-width toast)
	else if (mMaxWidth > 0 && W > mMaxWidth)
		W = mMaxWidth;

	// Position: centre-x and/or sit on a bottom baseline, else fixed x/y.
	int X = mCenterX ? (gr_fb_width() - W) / 2 : mPosX;
	int Y = (mBottom >= 0) ? (mBottom - H) : mPosY;

	// Slide-up intro: offset Y downward by the eased tween amount (decays to 0).
	Y += mIntro.value();

	mRenderX = X;
	mRenderY = Y;
	mRenderW = W;
	mRenderH = H;
	SetActionPos(X, Y, W, H);

	// Pass 2 - place children along the main axis, aligned on the cross axis.
	int crossStart = mHorizontal ? (Y + mPadding) : (X + mPadding);
	int cursor = mHorizontal ? (X + mPadding) : (Y + mPadding);
	bool first = true;
	for (size_t i = 0; i < n; i++) {
		if (!vis[i])
			continue;
		Child& c = mChildren[i];
		if (!first)
			cursor += mSpacing;
		first = false;

		int crossPos = crossStart;
		if (mAlign == 1)
			crossPos += (maxCross - crossSize[i]) / 2;
		else if (mAlign == 2)
			crossPos += (maxCross - crossSize[i]);

		int newX = mHorizontal ? cursor : crossPos;
		int newY = mHorizontal ? crossPos : cursor;

		// Only reposition when it actually moved (some widgets set their mUpdate
		// flag in SetRenderPos; calling it every frame would spin the GUI loop).
		int cx = 0, cy = 0, cw = 0, ch = 0;
		c.render->GetRenderPos(cx, cy, cw, ch);
		if (cx != newX || cy != newY) {
			// GUIImage::SetRenderPos rejects a non-zero w/h (it's position-only),
			// so images must be moved with 0,0 or they stay pinned at the origin.
			// Base/text objects keep their measured size when we pass w,h.
			if (dynamic_cast<GUIImage*>(c.obj))
				c.render->SetRenderPos(newX, newY, 0, 0);
			else
				c.render->SetRenderPos(newX, newY, cw, ch);
		}

		if (c.action) {
			int ax = 0, ay = 0, aw = 0, ah = 0;
			c.action->GetActionPos(ax, ay, aw, ah);
			if (ax != newX || ay != newY)
				c.action->SetActionPos(newX, newY,
						aw > 0 ? aw : mainSize[i], ah > 0 ? ah : crossSize[i]);
		}

		cursor += mainSize[i];
	}
}

int GUICard::Render(void)
{
	if (!isConditionTrue())
		return 0;

	Layout();
	if (mRenderW <= 0 || mRenderH <= 0)
		return 0;

	// Resolve the (possibly var-driven) outline colour for this frame.
	COLOR oc = mOutlineColor;
	if (mHasOutline && !mOutlineVar.empty()) {
		std::string cs = DataManager::GetStrValue(mOutlineVar);
		if (!cs.empty())
			ConvertStrToColor(cs, &oc);
	}

	// (Re)build the cached shape surfaces only when the geometry/colour changed -
	// during the slide only the blit position moves, so this is a no-op per frame.
	if (!mBgSurface || mCacheW != mRenderW || mCacheH != mRenderH ||
	    mCacheRadius != mRadius || !color_eq(mCacheBg, mBgColor)) {
		free_shape_surface(mBgSurface);
		mBgSurface = make_shape_surface(mRenderW, mRenderH, mRadius, 0, mBgColor);
		mCacheBg = mBgColor;
	}
	if (mHasOutline && (!mOutlineSurface || mCacheW != mRenderW || mCacheH != mRenderH ||
	    mCacheRadius != mRadius || mCacheStroke != mStroke || !color_eq(mCacheOutline, oc))) {
		free_shape_surface(mOutlineSurface);
		mOutlineSurface = make_shape_surface(mRenderW, mRenderH, mRadius, mStroke, oc);
		mCacheOutline = oc;
		mCacheStroke = mStroke;
	}
	mCacheW = mRenderW;
	mCacheH = mRenderH;
	mCacheRadius = mRadius;

	if (mBgSurface)
		gr_blit(mBgSurface, 0, 0, mRenderW, mRenderH, mRenderX, mRenderY);
	if (mHasOutline && mOutlineSurface)
		gr_blit(mOutlineSurface, 0, 0, mRenderW, mRenderH, mRenderX, mRenderY);

	for (size_t i = 0; i < mChildren.size(); i++) {
		if (mChildren[i].render)
			mChildren[i].render->Render();
	}
	return 0;
}

int GUICard::Update(void)
{
	if (!isConditionTrue())
		return 0;

	int ret = 0;
	for (size_t i = 0; i < mChildren.size(); i++) {
		if (mChildren[i].render) {
			int r = mChildren[i].render->Update();
			if (r < 0)
				LOGERR("A card child update request has failed.\n");
			else if (r > ret)
				ret = r;
		}
	}

	// Advance the intro animation; force a full redraw while it slides so the
	// background under the moving card is repainted each frame.
	if (mIntro.active()) {
		mIntro.step();
		ret = 2;
	}
	return ret;
}

int GUICard::NotifyTouch(TOUCH_STATE state, int x, int y)
{
	if (state == TOUCH_START) {
		mTouchChild = NULL;
		for (std::vector<Child>::reverse_iterator it = mChildren.rbegin(); it != mChildren.rend(); ++it) {
			if (it->action && it->obj && it->obj->isConditionTrue() && it->action->IsInRegion(x, y)) {
				mTouchChild = it->action;
				break;
			}
		}
	}
	if (mTouchChild)
		return mTouchChild->NotifyTouch(state, x, y);
	return 0;
}

void GUICard::SetPageFocus(int inFocus)
{
	// Restart the intro each time the card's page/overlay gains focus (the toast
	// overlay re-focuses on every gui_toast(), so the slide replays per toast).
	if (inFocus && mIntroAmount > 0)
		mIntro.start(mIntroFrames, mIntroAmount);

	for (size_t i = 0; i < mChildren.size(); i++) {
		if (mChildren[i].render)
			mChildren[i].render->SetPageFocus(inFocus);
	}
}
