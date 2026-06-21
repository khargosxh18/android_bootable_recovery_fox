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

// foxui.hpp - FoxUiEngine
//
// "FoxUiEngine" is the name of the OrangeFox XML UI engine: the libfoxui static
// library (the GUI* object classes, the page/template loader, DataManager
// binding, and the OrangeFox layout containers <column>/<row>/<scroll>),
// rendered on top of minuitwrp (a fork of AOSP's minui; the gr_* backend).
//
// The engine is inherited from TWRP, so the runtime class prefix stays GUI* and
// the gr_* prefix marks the minuitwrp boundary - we do NOT churn those names.
// This header just gives new OrangeFox-authored code a branded namespace to
// refer to the engine's entry points, without changing any existing type.
//
//   FoxUiEngine::PageManager::ChangePage("advanced");
//   class MyWidget : public FoxUiEngine::Object, public FoxUiEngine::Render { ... };
//
// It is purely additive (type aliases); including it is optional.

#ifndef _FOXUI_HPP
#define _FOXUI_HPP

#include "objects.hpp"
#include "pages.hpp"

namespace FoxUiEngine {

	// Page / navigation layer (pages.hpp)
	using Page        = ::Page;
	using PageSet     = ::PageSet;
	using PageManager = ::PageManager;

	// Object model (objects.hpp). GUI* stays the canonical name; these are
	// branded aliases for new code.
	using Object          = ::GUIObject;
	using Render          = ::RenderObject;
	using Action          = ::ActionObject;
	using Input           = ::InputObject;
	using ScrollContainer = ::GUIScrollContainer;

} // namespace FoxUiEngine

#endif // _FOXUI_HPP
