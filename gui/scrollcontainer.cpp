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

	You should have received a copy of the GNU General Public License
	along with TWRP.  If not, see <http://www.gnu.org/licenses/>.
*/

// scrollcontainer.cpp - FoxUiEngine GUIScrollContainer: a scrollable vertical
// viewport that auto-places (and scrolls) a set of adopted child objects. One
// of the OrangeFox FoxUiEngine layout containers (<column>/<row>/<scroll>). See
// the <scroll> handling in pages.cpp and the authoring notes in gui/AGENTS.md.

#include <stdlib.h>
#include <string>
#include <vector>

extern "C" {
#include "../twcommon.h"
}
#include "minuitwrp/minui.h"

#include "rapidxml.hpp"
#include "objects.hpp"

// Slop (in pixels) a finger may move before a press is treated as a scroll drag
// rather than a tap on a child. Kept small but non-zero to tolerate jitter.
#define SC_TOUCH_DEBOUNCE 12

GUIScrollContainer::GUIScrollContainer(xml_node<>* node) : GUIObject(node)
{
	mSpacing = 0;
	mPaddingTop = mPaddingBottom = 0;
	mDefaultItemH = 0;

	mScrollY = 0;
	mContentH = 0;
	mScrollingSpeed = 0;
	mOverscroll = 0;

	mTouchChild = NULL;
	mStartY = mLastY = mLast2Y = mTouchStartX = 0;
	mDragging = false;
	mTouchDebounce = SC_TOUCH_DEBOUNCE;
	mUpdate = false;
	mFocusChild = -1;
	mIntroEnabled = false;

	mScrollbarW = 0;
	mScrollbarColor.red = 128; mScrollbarColor.green = 128;
	mScrollbarColor.blue = 128; mScrollbarColor.alpha = 150;

	if (!node)
		return;

	// The viewport rect is read straight off the <scroll> element's attributes
	// (x/y/w/h), defaulting to the rest of the screen below y. Config attributes
	// (spacing/padding/itemheight) are read the same way; the element's child
	// *nodes* are the objects to scroll and are adopted by ProcessNode.
	mRenderX = 0;
	mRenderY = 0;
	mRenderW = gr_fb_width();
	mRenderH = gr_fb_height();
	LoadPlacement(node, &mRenderX, &mRenderY, &mRenderW, &mRenderH);
	if (!node->first_attribute("w"))
		mRenderW = gr_fb_width() - mRenderX;
	if (!node->first_attribute("h"))
		mRenderH = gr_fb_height() - mRenderY;

	// Rubber-band allowance for overscroll (snap-back) at either end.
	mOverscroll = mRenderH / 6;

	// Intro slide-in is opt-in (intro="1"); off by default so static pages
	// don't move on entry.
	mIntroEnabled = (LoadAttrString(node, "intro") == "1");

	mSpacing = LoadAttrIntScaleY(node, "spacing", 0);
	int pad = LoadAttrIntScaleY(node, "padding", 0);
	mPaddingTop = LoadAttrIntScaleY(node, "paddingtop", pad);
	mPaddingBottom = LoadAttrIntScaleY(node, "paddingbottom", pad);
	mDefaultItemH = LoadAttrIntScaleY(node, "itemheight", 0);

	// Scroll indicator: width in px (default 8dp, scrollbar="0" disables it),
	// colour overridable via scrollbarcolor.
	mScrollbarW = LoadAttrIntScaleX(node, "scrollbar", 8);
	mScrollbarColor = LoadAttrColor(node, "scrollbarcolor", mScrollbarColor);

	// Optional edge fade (e.g. edgefade="%background%"); off unless given.
	COLOR nofade;
	nofade.red = nofade.green = nofade.blue = nofade.alpha = 0;
	mEdgeFade = LoadAttrColor(node, "edgefade", nofade);

	// The whole viewport is the touch target so the page routes the entire
	// touch stream here; we re-dispatch taps to children ourselves.
	SetActionPos(mRenderX, mRenderY, mRenderW, mRenderH);
}

void GUIScrollContainer::Adopt(GUIObject* obj, RenderObject* ro, ActionObject* ao)
{
	Child c;
	c.obj = obj;
	c.render = ro;
	c.action = ao;
	c.layoutY = 0;
	c.layoutH = 0;
	c.layoutVisible = false;
	mChildren.push_back(c);
}

// Intrinsic advance of a child along the vertical axis.
static int sc_child_height(GUIScrollContainer::Child& c, int defaultH)
{
	// A scroll list self-sizes to show every item (no inner scrolling).
	GUIScrollList* sl = dynamic_cast<GUIScrollList*>(c.obj);
	if (sl)
		return sl->GetNaturalHeight();

	int cx = 0, cy = 0, cw = 0, ch = 0;
	if (c.render)
		c.render->GetRenderPos(cx, cy, cw, ch);

	// Text without an explicit bounding box reports no height - measure it.
	if (ch <= 0) {
		GUIText* t = dynamic_cast<GUIText*>(c.obj);
		if (t) {
			int tw = 0, th = 0;
			t->GetCurrentBounds(tw, th);
			ch = th;
		}
	}
	if (ch <= 0)
		ch = defaultH;
	return ch;
}

void GUIScrollContainer::Layout(void)
{
	// Pass 1 - measure total content height (and gather per-child heights /
	// visibility so the placement pass doesn't recompute them).
	size_t n = mChildren.size();
	std::vector<int> heights(n, 0);
	std::vector<bool> visible(n, false);

	int cursor = mPaddingTop;
	int visibleCount = 0;
	for (size_t i = 0; i < n; i++) {
		Child& c = mChildren[i];
		// A pure-action child (no renderable) takes no layout space.
		bool vis = c.render && (c.obj ? c.obj->isConditionTrue() : true);
		visible[i] = vis;
		if (!vis)
			continue;
		int h = sc_child_height(c, mDefaultItemH);
		heights[i] = h;
		if (visibleCount > 0)
			cursor += mSpacing;
		cursor += h;
		visibleCount++;
	}
	mContentH = cursor + mPaddingBottom;

	// Clamp the scroll offset. Overscroll (rubber-band) is only allowed when the
	// content actually overflows; if it fits, the offset is pinned to 0 so the
	// page can't be dragged/bounced at all.
	int maxScroll = mContentH - mRenderH;
	if (maxScroll < 0)
		maxScroll = 0;
	int over = (maxScroll > 0) ? mOverscroll : 0;
	if (mScrollY > maxScroll + over)
		mScrollY = maxScroll + over;
	if (mScrollY < -over)
		mScrollY = -over;

	// Pass 2 - place each visible child. We track each child's laid-out rect so
	// the container can hit-test taps itself (ActionObject::SetActionPos rejects
	// negative coords, so a scrolled-out child can't be parked via its own hit
	// region - we don't rely on it).
	int place = mPaddingTop;
	bool first = true;
	for (size_t i = 0; i < n; i++) {
		Child& c = mChildren[i];
		if (!visible[i]) {
			c.layoutVisible = false;
			continue;
		}
		if (!first)
			place += mSpacing;
		first = false;

		// mIntro.value() slides content down a little on entry, easing to 0.
		int drawY = mRenderY + place - mScrollY + mIntro.value();
		int h = heights[i];

		c.layoutVisible = true;
		c.layoutY = drawY;
		c.layoutH = h;

		if (c.render) {
			int cx = 0, cy = 0, cw = 0, ch = 0;
			c.render->GetRenderPos(cx, cy, cw, ch);
			// Only the Y axis is managed; keep the child's own X and width.
			// IMPORTANT: only reposition when something actually changed.
			// SetRenderPos on some widgets (e.g. GUIScrollList) sets their
			// mUpdate flag, so calling it unconditionally every frame would make
			// them perpetually request a redraw -> 100% CPU. SetRenderPos has no
			// negative guard, so a list's item hit-testing (which uses mRenderY)
			// stays correct even when the child is partly scrolled off-top.
			if (cy != drawY || ch != h)
				c.render->SetRenderPos(cx, drawY, cw, h);
		}

		place += h;
	}
}

int GUIScrollContainer::Render(void)
{
	if (!isConditionTrue())
		return 0;

	Layout();

	// Confine all child drawing (including a child's own gr_clip) to the
	// viewport, then restore the previous clip.
	gr_clip_push(mRenderX, mRenderY, mRenderW, mRenderH);
	for (size_t i = 0; i < mChildren.size(); i++) {
		if (mChildren[i].render)
			mChildren[i].render->Render();
	}
	gr_clip_pop();

	// Scroll indicator on the right edge (only when the content overflows).
	// Drawn after gr_clip_pop so the thumb is never clipped by the viewport.
	if (mScrollbarW > 0 && mContentH > mRenderH) {
		int trackH = mRenderH;
		int thumbH = (int)((long)mRenderH * mRenderH / mContentH);
		int minThumb = mRenderH / 10;
		if (thumbH < minThumb)
			thumbH = minThumb;
		int maxScroll = mContentH - mRenderH;
		int thumbY = mRenderY + (maxScroll > 0 ? (int)((long)mScrollY * (trackH - thumbH) / maxScroll) : 0);
		int gap = mScrollbarW / 2;
		int thumbX = mRenderX + mRenderW - mScrollbarW - gap;
		gr_color(mScrollbarColor.red, mScrollbarColor.green, mScrollbarColor.blue, mScrollbarColor.alpha);
		gr_fill(thumbX, thumbY, mScrollbarW, thumbH);
	}

	// Optional edge fade hinting more content above/below (opt-in via edgefade).
	if (mEdgeFade.alpha > 0 && mContentH > mRenderH) {
		int maxScroll = mContentH - mRenderH;
		int fadeH = mRenderH / 8;
		if (fadeH > 0) {
			if (mScrollY > 0) {
				for (int i = 0; i < fadeH; i++) {
					int a = mEdgeFade.alpha * (fadeH - i) / fadeH;
					gr_color(mEdgeFade.red, mEdgeFade.green, mEdgeFade.blue, a);
					gr_fill(mRenderX, mRenderY + i, mRenderW, 1);
				}
			}
			if (mScrollY < maxScroll) {
				for (int i = 0; i < fadeH; i++) {
					int a = mEdgeFade.alpha * (fadeH - i) / fadeH;
					gr_color(mEdgeFade.red, mEdgeFade.green, mEdgeFade.blue, a);
					gr_fill(mRenderX, mRenderY + mRenderH - 1 - i, mRenderW, 1);
				}
			}
		}
	}
	return 0;
}

int GUIScrollContainer::Update(void)
{
	if (!isConditionTrue())
		return 0;

	int ret = 0;

	// Advance the intro slide-in, if running.
	if (mIntro.active()) {
		mIntro.step();
		mUpdate = true;
	}

	// Kinetic scrolling: keep moving in the fling direction, decaying to rest.
	if (mScrollingSpeed != 0) {
		mScrollY -= mScrollingSpeed;
		if (mScrollingSpeed > 0)
			mScrollingSpeed = mScrollingSpeed * 3 / 4 - 1;
		else
			mScrollingSpeed = mScrollingSpeed * 3 / 4 + 1;
		mUpdate = true;
	}

	Layout(); // re-place for the new offset / any visibility change, re-clamps

	int maxScroll = mContentH - mRenderH;
	if (maxScroll < 0)
		maxScroll = 0;

	// Stop the fling once we run past either end (the bounce-back takes over).
	if (mScrollY <= 0 || mScrollY >= maxScroll)
		mScrollingSpeed = 0;

	// Rubber-band: when not actively dragging, ease any overscroll back into
	// range. mTouchChild==NULL && !mDragging means no finger is down.
	if (!mDragging) {
		if (mScrollY < 0) {
			mScrollY = mScrollY * 3 / 4;
			if (mScrollY > -2)
				mScrollY = 0;
			mUpdate = true;
		} else if (mScrollY > maxScroll) {
			int over = (mScrollY - maxScroll) * 3 / 4;
			if (over < 2)
				over = 0;
			mScrollY = maxScroll + over;
			mUpdate = true;
		}
	}

	if (mUpdate) {
		ret = 2; // request a full redraw
		mUpdate = false;
	}

	for (size_t i = 0; i < mChildren.size(); i++) {
		if (mChildren[i].render) {
			int r = mChildren[i].render->Update();
			if (r < 0)
				LOGERR("A scroll child update request has failed.\n");
			else if (r > ret)
				ret = r;
		}
	}
	return ret;
}

int GUIScrollContainer::NotifyTouch(TOUCH_STATE state, int x, int y)
{
	switch (state) {
		case TOUCH_START:
			mScrollingSpeed = 0; // a new touch stops any fling
			mStartY = mLastY = mLast2Y = y;
			mTouchStartX = x;
			mDragging = false;
			// Remember which child is under the finger as the potential tap
			// target, but don't notify it yet - we only deliver a tap on
			// release if the finger didn't turn into a scroll drag. Hit-test
			// against the laid-out geometry, clamped to the viewport, so a
			// scrolled-out child never matches.
			mTouchChild = NULL;
			for (std::vector<Child>::reverse_iterator it = mChildren.rbegin(); it != mChildren.rend(); ++it) {
				if (!it->layoutVisible || !it->action)
					continue;
				int top = it->layoutY > mRenderY ? it->layoutY : mRenderY;
				int bot = (it->layoutY + it->layoutH) < (mRenderY + mRenderH) ? (it->layoutY + it->layoutH) : (mRenderY + mRenderH);
				if (y >= top && y < bot) {
					mTouchChild = it->action;
					break;
				}
			}
			// Forward the press so the child shows its highlight/press state. If
			// the gesture turns into a scroll we cancel it (see TOUCH_DRAG).
			if (mTouchChild)
				mTouchChild->NotifyTouch(TOUCH_START, x, y);
			return 0;

		case TOUCH_DRAG: {
			int delta = y - mLastY;
			mLast2Y = mLastY;
			mLastY = y;
			if (!mDragging && abs(y - mStartY) > mTouchDebounce) {
				mDragging = true;
				// Became a scroll: cancel the child's press so it neither stays
				// highlighted nor fires on release. A far-away drag forces a list
				// to deselect (drag exceeds its debounce) and a button to drop
				// its press (finger left the region).
				if (mTouchChild) {
					int fy = (y >= mStartY) ? (mStartY + 100000) : (mStartY - 100000);
					mTouchChild->NotifyTouch(TOUCH_DRAG, x, fy);
					mTouchChild = NULL;
				}
			}
			int maxScroll = mContentH - mRenderH;
			if (maxScroll < 0)
				maxScroll = 0;
			// Only scroll/overscroll when the content actually overflows. A drag
			// on a page that fits still cancels the tap (above) but moves nothing.
			if (mDragging && maxScroll > 0) {
				mScrollY -= delta;
				if (mScrollY < -mOverscroll)
					mScrollY = -mOverscroll;
				if (mScrollY > maxScroll + mOverscroll)
					mScrollY = maxScroll + mOverscroll;
				mUpdate = true;
			}
			return 0;
		}

		case TOUCH_RELEASE:
			if (!mDragging && mTouchChild) {
				// A tap: the child already got TOUCH_START on press, so just
				// release it to fire its action (and clear the press highlight).
				mTouchChild->NotifyTouch(TOUCH_RELEASE, x, y);
			} else if (mDragging) {
				// Fling with the velocity of the last finger movement - but only
				// when there is something to scroll.
				int maxScroll = mContentH - mRenderH;
				if (maxScroll > 0) {
					mScrollingSpeed = mLastY - mLast2Y;
					if (abs(mScrollingSpeed) < mTouchDebounce)
						mScrollingSpeed = 0;
				}
			}
			mTouchChild = NULL;
			mDragging = false;
			return 0;

		default:
			return 0;
	}
}

int GUIScrollContainer::NotifyVarChange(const std::string& varName, const std::string& value)
{
	GUIObject::NotifyVarChange(varName, value);
	// A condition change may add/remove a child or resize a list - re-layout.
	mUpdate = true;
	return 0;
}

void GUIScrollContainer::SetPageFocus(int inFocus)
{
	if (inFocus) {
		// Entering the page: start at the top, no residual fling/highlight, and
		// kick off a subtle content slide-in.
		mScrollY = 0;
		mScrollingSpeed = 0;
		mFocusChild = -1;
		if (mIntroEnabled)
			mIntro.start(8, mRenderH / 12);
		mUpdate = true;
	} else {
		ClearChildFocus();
	}
	for (size_t i = 0; i < mChildren.size(); i++) {
		if (mChildren[i].render)
			mChildren[i].render->SetPageFocus(inFocus);
	}
}

// ---- Hardware-key navigation -------------------------------------------------
//
// The container presents itself to Page::MoveFocusIndex as a single scrollable
// list whose "items" are the focusable items of its interactive children
// (anything implementing IInteractiveScrollList, i.e. the embedded listboxes).
// We chain selection across children and auto-scroll so the highlighted item
// stays inside the viewport. SelectFocusedElement activates via the normal tap
// path (GetFocusedItemActionPos -> NotifyTouch on this container).

// Interactive (focusable) interface of a child, or NULL if it isn't one /
// isn't currently visible / is empty.
static IInteractiveScrollList* sc_interactive(const GUIScrollContainer::Child& c)
{
	if (!c.obj || !c.obj->isConditionTrue())
		return NULL;
	IInteractiveScrollList* il = dynamic_cast<IInteractiveScrollList*>(c.obj);
	if (il && il->GetItemCount() > 0)
		return il;
	return NULL;
}

int GUIScrollContainer::FirstFocusableChild(void) const
{
	for (size_t i = 0; i < mChildren.size(); i++)
		if (sc_interactive(mChildren[i]))
			return (int)i;
	return -1;
}

int GUIScrollContainer::NextFocusableChild(int after) const
{
	for (int i = after + 1; i < (int)mChildren.size(); i++)
		if (sc_interactive(mChildren[i]))
			return i;
	return -1;
}

int GUIScrollContainer::PrevFocusableChild(int before) const
{
	for (int i = before - 1; i >= 0; i--)
		if (sc_interactive(mChildren[i]))
			return i;
	return -1;
}

void GUIScrollContainer::ClearChildFocus(void)
{
	if (mFocusChild >= 0 && mFocusChild < (int)mChildren.size() && mChildren[mFocusChild].action)
		mChildren[mFocusChild].action->SetFocus(0);
	mFocusChild = -1;
}

void GUIScrollContainer::EnsureFocusVisible(void)
{
	if (mFocusChild < 0 || mFocusChild >= (int)mChildren.size())
		return;
	ActionObject* ao = mChildren[mFocusChild].action;
	if (!ao)
		return;

	int ix, iy, iw, ih;
	if (!ao->GetFocusedItemActionPos(ix, iy, iw, ih))
		return; // iy/ih are screen-space (child mRenderY already folds in scroll)

	int margin = mSpacing;
	if (iy < mRenderY + margin)
		mScrollY -= (mRenderY + margin - iy);
	else if (iy + ih > mRenderY + mRenderH - margin)
		mScrollY += (iy + ih) - (mRenderY + mRenderH - margin);

	if (mScrollY < 0)
		mScrollY = 0;
	int maxScroll = mContentH - mRenderH;
	if (maxScroll < 0)
		maxScroll = 0;
	if (mScrollY > maxScroll)
		mScrollY = maxScroll;

	mScrollingSpeed = 0;
	mUpdate = true;
	Layout(); // reposition for the new offset so a follow-up query is correct
}

size_t GUIScrollContainer::GetItemCount()
{
	size_t total = 0;
	for (size_t i = 0; i < mChildren.size(); i++) {
		IInteractiveScrollList* il = sc_interactive(mChildren[i]);
		if (il)
			total += il->GetItemCount();
	}
	return total;
}

void GUIScrollContainer::SetSelectedItem(size_t index)
{
	ClearChildFocus();
	size_t acc = 0;
	for (size_t i = 0; i < mChildren.size(); i++) {
		IInteractiveScrollList* il = sc_interactive(mChildren[i]);
		if (!il)
			continue;
		size_t cnt = il->GetItemCount();
		if (index < acc + cnt) {
			mFocusChild = (int)i;
			il->SetSelectedItem(index - acc);
			if (mChildren[i].action)
				mChildren[i].action->SetFocus(1);
			EnsureFocusVisible();
			return;
		}
		acc += cnt;
	}
}

bool GUIScrollContainer::MoveSelectionDown()
{
	if (mFocusChild < 0) {
		int f = FirstFocusableChild();
		if (f < 0)
			return false;
		mFocusChild = f;
		sc_interactive(mChildren[f])->SetSelectedItem(0);
		if (mChildren[f].action)
			mChildren[f].action->SetFocus(1);
		EnsureFocusVisible();
		return true;
	}

	IInteractiveScrollList* cur = sc_interactive(mChildren[mFocusChild]);
	if (cur && cur->MoveSelectionDown()) {
		EnsureFocusVisible();
		return true;
	}

	int nxt = NextFocusableChild(mFocusChild);
	if (nxt < 0)
		return false; // past the last item - let the page move focus out

	if (mChildren[mFocusChild].action)
		mChildren[mFocusChild].action->SetFocus(0);
	mFocusChild = nxt;
	sc_interactive(mChildren[nxt])->SetSelectedItem(0);
	if (mChildren[nxt].action)
		mChildren[nxt].action->SetFocus(1);
	EnsureFocusVisible();
	return true;
}

bool GUIScrollContainer::MoveSelectionUp()
{
	if (mFocusChild < 0) {
		int f = PrevFocusableChild((int)mChildren.size());
		if (f < 0)
			return false;
		mFocusChild = f;
		IInteractiveScrollList* il = sc_interactive(mChildren[f]);
		il->SetSelectedItem(il->GetItemCount() - 1);
		if (mChildren[f].action)
			mChildren[f].action->SetFocus(1);
		EnsureFocusVisible();
		return true;
	}

	IInteractiveScrollList* cur = sc_interactive(mChildren[mFocusChild]);
	if (cur && cur->MoveSelectionUp()) {
		EnsureFocusVisible();
		return true;
	}

	int prv = PrevFocusableChild(mFocusChild);
	if (prv < 0)
		return false;

	if (mChildren[mFocusChild].action)
		mChildren[mFocusChild].action->SetFocus(0);
	mFocusChild = prv;
	IInteractiveScrollList* il = sc_interactive(mChildren[prv]);
	il->SetSelectedItem(il->GetItemCount() - 1);
	if (mChildren[prv].action)
		mChildren[prv].action->SetFocus(1);
	EnsureFocusVisible();
	return true;
}

int GUIScrollContainer::GetFocusedItemActionPos(int& x, int& y, int& w, int& h)
{
	if (mFocusChild < 0 || mFocusChild >= (int)mChildren.size())
		return 0;
	ActionObject* ao = mChildren[mFocusChild].action;
	if (!ao)
		return 0;
	return ao->GetFocusedItemActionPos(x, y, w, h);
}

void GUIScrollContainer::SetFocus(bool focus)
{
	ActionObject::SetFocus(focus);
	// Mirror focus onto the selected child so its row highlights - but DO NOT
	// reset mFocusChild here. Page::SetFocus re-asserts focus on every key press
	// that stays on this container (it calls SetFocus(0) then SetFocus(1) on the
	// same element); clearing the selection in the SetFocus(0) leg would bounce
	// navigation back to the first item on the next press.
	if (mFocusChild >= 0 && mFocusChild < (int)mChildren.size() && mChildren[mFocusChild].action)
		mChildren[mFocusChild].action->SetFocus(focus);
}
