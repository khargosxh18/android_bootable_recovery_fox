# FoxUiEngine — building UI

This file is the authoring guide for **FoxUiEngine**, the OrangeFox UI engine.
Read it before writing or modifying anything under `gui/`. For the broader
project orientation, see the root `AGENTS.md`.

**FoxUiEngine** is the **XML-themed**, immediate-mode-ish UI engine built here as
the static library **`libfoxui`** (formerly `libguitwrp`) and rendered by
`minuitwrp` (a fork of AOSP `minui`; pixelflinger/GGL, the `gr_*` backend).
Almost everything visible is defined in XML under `gui/theme/<theme>/` (the
active theme is `portrait_hdpi`); C++ only provides the object classes (the
`GUI*` prefix is the canonical, TWRP-inherited name), the action handlers, and
the `DataManager` values that drive it. New OrangeFox code can refer to the
engine's entry points through the branded `FoxUiEngine::` aliases in
[`foxui.hpp`](foxui.hpp).

## How a theme is assembled

`gui/theme/portrait_hdpi/ui.xml` is the **manifest**. It `<include>`s, in order:
the theme overrides (`themes/style.xml`, `font.xml`, `accent.xml`, `navbar.xml`),
the **resources** (`resources/vars.xml`, `images.xml`, `styles.xml`), the
**templates** (`pages/templates/*.xml`), then every **page** (`pages/main.xml`,
`pages/advanced.xml`, ...). At build time `gui/libfoxui_defaults.go` copies the
selected theme folder into the ramdisk as `/twres` (the `TWRES="/twres/"` cflag),
and the page loader reads everything from there.

So a new page is "registered" simply by adding `<xml name="/twres/pages/yourpage.xml"/>`
to `ui.xml` — there is no separate page registry.

## The resource layer (define before you reference)

Everything is referenced by name. Define these once, use them everywhere:

- **Variables** — `resources/vars.xml`: `<variable name="row1_1_y" value="300+%cutout_w%"/>`.
  These are **placement coordinates and colors**, evaluated as arithmetic over other
  vars. Reference anywhere as `%row1_1_y%`. The whole layout is a grid of named row/col
  variables (`row1_1_y`, `row1_2_y`, … `row9_1_y`; `col1_x`, `col1_x_indent`,
  `col2_x`, `center_x`, `screen_w`, `screen_h`, `ab_h`, `cutout_w`, …). **Reuse
  existing row/col vars** rather than hard-coding pixel numbers — they already fold
  in the camera cutout (`%cutout_w%`) and scale across the notch. Colors are vars too
  (`%accent%`, `%text%`, `%background%`, `%transparent%`, `%text_fail%`).
- **Images** — `resources/images.xml`:
  - `<image name="partitions" filename="SVG/Icons/partitions.svg" retainaspect="1" scale="3.25" tint="#413327"/>`
    — raster or SVG (SVG is rasterized via nanosvg at load).
  - `<shape name="btn_raised" w="%btn_w%" h="%btn_h%" radius="-1" color="%neutral_light%"/>`
    — a drawn rounded rectangle (radius `-1` = sharp corners). Use shapes for cards,
    pills, button backgrounds so they theme correctly.
  - `<animation name="progress" filename="Light/Progress/indeterminate"/>` — frame strip.
  Reference via `resource="partitions"` / `<image resource="btn_raised"/>`.
- **Styles** — `resources/styles.xml`: a named bundle of child nodes an object
  inherits. `<style name="text_ab_title"><font resource="Secondary-title" color="%actbar_color%"/></style>`.
  Apply with `<text style="text_ab_title">…</text>`. The node lookup is by child name
  (a `<text style="X">` looks for `<font>` inside style `X`), so a style can set font,
  fill, image, limit, etc. at once. Color/font **theme variants** live in
  `themes/styles/{Dark,Light,Gray,Cream,Black}.xml` (these define `%text%`, `%accent%`,
  the font resources, etc.) — pick values from the vars, never hard-code a hex that
  should change with the theme.
- **Strings** — `common/languages/en.xml` (+ translations in sibling files and
  `extra-languages/`): `<string name="install_btn">Install</string>`. Strings
  can themselves embed `%var%` and `{@other_string}`. Reference as `{@install_btn}`
  (preferred, with optional default `{@id=default text}`) or `%@id%`.

## Text expansion rules (`gui_parse_text`, `gui/gui.cpp:855`)

In any text/attribute value, two substitution passes run:
1. `{@name}` → string resource `name`. `{@name=fallback}` → resource with fallback.
2. `%token%` → if token starts with `@`, string resource `token[1:]`; else the
   `DataManager` value named `token`. `%%` is a literal `%`.

So `<text>{@decrypt_data_enter_pass}</text>`, `<text>%tw_zip_location%</text>`
are all valid.

## Page structure

A page is a `<page name="…">` containing an ordered list of nodes, drawn top-to-bottom
(later = on top). Typical skeleton:

```xml
<page name="myfeature">
  <!-- 1. on-enter actions: set defaults, refresh state -->
  <action>
    <action function="set">tw_back=advanced</action>
    <action function="myfeature_refresh"/>
  </action>

  <!-- 2. include shared chrome via templates -->
  <template name="base"/>          <!-- statusbar, navbar, background, keys -->

  <!-- 3. action bar title -->
  <text style="text_ab_title">
    <placement x="%col1_x_indent%" y="%ab_bc_y%"/>
    <text>{@myfeature_title}</text>
  </text>

  <!-- 4. a toggle card: two variants gated by a condition -->
  <image><condition var1="myfeature_enabled" var2="1"/>
    <placement x="40" y="%ab_h%"/><image resource="main_switch_card_active"/></image>
  <button style="bs_btn"><condition var1="myfeature_enabled" var2="1"/>
    <placement x="0" y="%ab_h%" w="%screen_w%" h="%ab_btn_h%"/>
    <action function="myfeature_disable"/>
    <action function="page">myfeature</action>
  </button>
  <!-- ...the "off" variant with op="!=" ...-->

  <!-- 5. content rows, listboxes, inputs, etc. -->
</page>
```

**Node → class map** (handled in `gui/pages.cpp:ProcessNode`): `text→GUIText`,
`image→GUIImage`, `fill→GUIFill`, `button→GUIButton`, `checkbox→GUICheckbox`,
`action→GUIAction`, `console→GUIConsole`, `terminal→GUITerminal`, `slider→GUISlider`,
`slidervalue→GUISliderValue`, `listbox→GUIListBox`, `fileselector→GUIFileSelector`,
`partitionlist→GUIPartitionList`, `input→GUIInput`, `keyboard→GUIKeyboard`,
`animation→GUIAnimation`, `progressbar→GUIProgressBar`, `spinningimage→GUISpinningImage`,
`patternpassword→GUIPatternPassword`, `battery→GUIBattery`,
`template→(inline expansion of a named template)`. See each class in `objects.hpp`
for the exact child nodes it reads (`<placement>`, `<font>`, `<image>`, `<fill>`,
`<data>`, `<action>`, …).

**Terminal note:** `GUITerminal` (`gui/terminal.cpp`) is the on-device terminal on
the `terminal` page. Its back-end (`TerminalEngine`) wraps **libvterm**
(`external/libvterm`, linked as a static lib — see the guard in `orangefox.mk` and
`LOCAL_STATIC_LIBRARIES += libvterm` in `Android.mk`): libvterm owns the terminal
state machine, cell grid, scrollback, colours and attributes; the GUI side only
renders cells (`RenderItem`) and forwards input. The PTY child is launched with
`TERM=xterm-256color`, and the terminfo DB is shipped to `/system/etc/terminfo`.
This is separate from the web terminal (`ttyd`), which is its own browser-based
process — see `docs/remote-control.md`.

## Placement

Every visible node takes a `<placement x= y= w= h= placement=/>`. Coordinates are the
vars from `vars.xml`. The `placement` attribute is an anchor code (1–9, like a numpad):
e.g. `placement="4"` = center-x, `placement="5"` = centered both, `placement="1"` =
right-aligned. Omit `w`/`h` for intrinsic size (text/images).

### Auto-placement: `<column>` / `<row>` (prototype)

Instead of nailing every node to an absolute `x/y`, you can wrap a group in a
**layout container** that places its children along one axis. The container is not
itself a `GUIObject` — its children are registered as normal page objects (render
order, focus and touch all work as usual); the loader just rewrites their positions
after construction (`Page::LayoutContainer`, `gui/pages.cpp`).

```xml
<column x="%col1_x_indent%" y="%row1_1_y%" spacing="14" itemheight="48">
  <text style="caption"><text>First row</text></text>
  <text style="caption"><text>Second row</text></text>
  <button style="bs_btn"><placement w="%screen_w%" h="%ab_btn_h%"/>
    <action function="page">advanced</action></button>
</column>

<row x="%col1_x_indent%" y="%row6_1_y%" spacing="40" itemwidth="120">
  <text style="caption"><text>Left</text></text>
  <text style="caption"><text>Mid</text></text>
</row>
```

Container attributes: `x`/`y` (origin), `spacing` (gap between items on the axis),
`itemheight`/`itemwidth` (fallback advance), and `direction="vertical|horizontal"`
(overrides the element default, so `<column direction="horizontal">` is valid).

Rules of thumb:
- **Children omit `x`/`y`** — the container assigns them. They still need a
  `<placement w= h=>` for their *size* (buttons/images/shapes); only the position
  is taken over.
- **Advance per item** = the object's intrinsic size on the axis (image/button
  `w`/`h`, or a text's measured bounds), falling back to `itemheight`/`itemwidth`.
- **Known limitation:** `<column>`/`<row>` layout is computed once at page load and
  is *static* — a child hidden by a `<condition>` still reserves its slot (no reflow).
  Per-axis only; no nesting/centering/wrapping. For dynamic reflow + scrolling, use
  `<scroll>` (below). See `pages/column_demo.xml` for a live example
  (`<action function="page">column_demo</action>`).

### Scrollable viewport: `<scroll>`

`<scroll>` is the dynamic, scrollable big brother of `<column>`. It is a real
container object (`GUIScrollContainer`, `gui/scrollcontainer.cpp`) that **owns** its
children: it auto-stacks them vertically, *reflows* when a child's `<condition>`
toggles or a list grows/shrinks, clips them to its viewport, and **scrolls the whole
stack** (drag + kinetic fling) when the content is taller than the viewport.

```xml
<scroll x="0" y="%ab_h%" w="%screen_w%" h="%bl_fullscreen_h%"
        spacing="20" paddingtop="10" paddingbottom="40" itemheight="48">
  <text style="caption"><placement x="%gl_text_x%" y="0"/><text>{@group_main}</text></text>
  <listbox style="group_list"><placement x="0" y="0" w="%screen_w%" h="0"/> … </listbox>
  <text style="caption"><placement x="%gl_text_x%" y="0"/><text>{@group_more}</text></text>
  <listbox style="group_list"><placement x="0" y="0" w="%screen_w%" h="0"/> … </listbox>
</scroll>
```

- **Viewport** = the `<scroll>` element's own `x/y/w/h` attributes (not a child
  `<placement>`); `w`/`h` default to the rest of the screen. Config attributes:
  `spacing`, `padding` (or `paddingtop`/`paddingbottom`), `itemheight` (fallback advance).
- **Children manage Y only** — keep each child's own `x`/`w` (and `<placement>` for
  size); the container assigns Y. A child's `y` is ignored.
- **Embedded `<listbox>`/lists self-size**: the container sets a list to its full
  natural height (`GetNaturalHeight()`) so the *page* scrolls, not the list — no
  nested scrolling. Just give the list any `h` (it's overridden).
- **Conditions reflow**: hiding a child (or a list item) collapses its space and
  everything below moves up — this is the fix for "group alignment depends on item
  count". Put fixed chrome (action bar, navbar, gestures) *outside* the `<scroll>`.
- **Live example:** `pages/advanced.xml` (the Log / Fox-addons groups).
- **Hardware-key nav:** supported — the viewport chains its embedded lists into one
  vol-up/down selection sequence and auto-scrolls the highlighted row into view
  (`of_hw_control_mode`). Touch shows the normal press highlight, and a drag scrolls.
- **Current limitations:** vertical only; only `IInteractiveScrollList` children
  (listboxes) are hw-navigable — standalone `<button>` children and on-screen
  keyboard/`<input>` focus inside the viewport are not. Nested clipping is handled via
  `gr_clip_push/pop` (`minuitwrp/graphics.cpp`).

## FoxUiEngine extensions — quick reference

OrangeFox additions on top of the inherited TWRP engine. Most are parsed in
`gui/pages.cpp` (`ProcessNode` / `LoadPlacement` / `FindNode`); widgets are their
own `.cpp`.

**Placement & units**
- **Percent units:** `x/y/w/h="50%"` → 50% of the framebuffer dimension (distinct
  from `%var%` references).
- **Relative/anchor:** give an element `id="hdr"`, then another `y="below:hdr+12"`,
  `x="rightof:hdr"`, or align edges with `top:`/`bottom:`/`left:`/`right:`. Resolved
  at load against earlier elements; don't anchor into a layout container's children.
- **Safe-area:** `cutout="1"` on any placement pushes `y` down by `%cutout_w%`.

**Layout containers** (children omit `x`/`y`; the container assigns position)
- **`<column>` / `<row>`:** static auto-stacking (`spacing`, `itemheight/itemwidth`).
  Cross-axis `align="start|center|end"` (needs the container's `w`/`h`), and per-child
  `weight="N"` for flex along the main axis (needs the main-axis size on the container).
- **`<scroll>`:** scrollable viewport (see above). It only scrolls/overscrolls when
  the content actually overflows (a page that fits stays put — no drag, bounce or
  scrollbar). A scroll indicator (`scrollbar=`, `scrollbarcolor=`), optional
  `edgefade=`, and an opt-in intro slide-in (`intro="1"`, off by default).
- **`<card x= y= w= h= color="%cards_bg%" padding=>`:** a themed background panel
  with its children auto-stacked inside (inset by `padding`).
- **`<spacer h="24"/>`** reserves space; **`<divider/>`** draws a thin themed rule —
  both for use inside the containers above.
- **`<when minw= maxw= minh= maxh= orientation=>`:** responsive breakpoint; its
  children load only when the framebuffer matches.

**Widgets**
- **`<toggle var="fox_x"><text>Label</text><action .../></toggle>`:** a labelled
  on/off switch bound to a DataManager variable (tap flips it + runs child actions).

**Theming & tokens**
- **Design tokens:** `%space_xs/s/m/l/xl%`, `%radius_s/m/l%` — prefer over raw px.
- **Style inheritance:** `<style name="b" extends="a">` inherits a's nodes (b wins).

**Feedback & motion**
- **Toast:** `<action function="toast">message</action>` → transient bottom overlay
  (auto-dismiss ~3s, tap to dismiss). Needs `<page name="toast">` (in `dialogs.xml`).
- **Tweens:** `FoxUiEngine::Tween` (`gui/tween.hpp`) — frame-based eased offset for
  intro/exit slides (used by `<scroll>`).

**Authoring safety / tooling**
- **Load-time validation:** an explicit `style="X"` that doesn't resolve logs a
  precise `FoxUiEngine:` error at parse.
- **Headless linter/preview:** `gui/tools/foxui_lint.py` checks `style=`/`{@string}`
  refs across the theme on the host, and `--svg PAGE` emits a placement-box preview.

New OrangeFox code can use the `FoxUiEngine::` aliases in `foxui.hpp`.

## Conditions (show/hide/enable)

Any node accepts zero or more `<condition var1="…" [op="…"] var2="…"/>` children.
**All conditions on a node must be true** for it to render/act. Operators: `=` / `!=`
(string compare, default when `op` omitted is `=`), `>`, `<`, `>=`, `<=` (numeric),
and the special `op="modified"` (fires once when `var1` changes — used to trigger an
action on value change). Two pseudo-`var1` values: `fileexists` (true if `var2` path
exists) and `mounted` (true if `var2` is mounted). `var2` may itself be a `%var%` or
`{@string}`. Examples from the tree:
```xml
<condition var1="tw_is_encrypted" var2="1"/>
<condition var1="tw_is_decrypted" op="!=" var2="1"/>
<condition var1="tw_file_location1" op="modified"/>
```

## Actions (wire buttons to C++)

`<action function="name">arg</action>` invokes a registered `GUIAction` handler.
Handlers are registered in the `GUIAction` ctor (`gui/action.cpp` ~line 217) via
`ADD_ACTION(name)` or `ADD_ACTION_EX("xml_name", cpp_func)`. A `<button>` can carry
several `<action>`s run in order. Built-ins you will use constantly:

- `set` — write a DataManager value: `<action function="set">tw_back=advanced</action>`
- `page` — navigate: `<action function="page">advanced</action>`
- `overlay` / `overlay` with arg — show/dismiss a popup page
- `key` — simulate a key: `<action function="key">back</action>`
- `mount`/`unmount`, `flash`, `wipe`, `decrypt`, `fileexists`, `screenshot`, …

For new behavior, add `int GUIAction::myfeature_refresh(std::string arg)` in
`action.cpp`, register it with `ADD_ACTION(myfeature_refresh)`, then call it from XML.
Handlers are thin wrappers over recovery functions (`PartitionManager.Mount()`, …).

Long-running handlers use `operation_start("Title")` / `operation_end(status)` and
report via `gui_msg("id=Human readable text with {1} slots")` (the string id is
auto-loaded from the language files). Honor `simulate` / `tw_simulate_actions` for the
test mode. The action runs on the **action thread**, serialized by `tw_busy` — never
run two engine-mutating actions concurrently.

## Templates (reuse page fragments)

A `<template name="x">…nodes…</template>` (defined in `pages/templates/templates.xml`
or inline) is inlined wherever `<template name="x"/>` appears. Use these to share
chrome: `base` (statusbar+navbar+background+keys), `ab` / `ab_main` (action bar),
`navbar`, `navbar_home` / `navbar_console` / `navbar_key`, `gestures`, `statusbarinfo`,
`body_nav`, `keys`, `base_key`, `gestures_key`, etc. Templates can nest. Define new
ones for any repeated layout. Template expansion is capped at depth 10 (recursive
templates error out).

## Data binding (the feedback loop)

UI state lives in `DataManager` (`data.hpp`) as named string/int values. The contract
for any feature is:

1. Pick variable names (e.g. `tw_myfeature_enabled`, `myfeature_list_count`).
2. A `GUIAction` sets/updates them: `DataManager::SetValue("tw_myfeature_enabled", 1)`.
3. The XML reads them: gate visibility with `<condition>`, show values with `%var%`,
   and react to changes with `op="modified"`.

Many values are seeded from `vars.xml` (which sets defaults + persistence: a
`<variable … persist="1"/>` is saved across reboots in Fox settings). Existing
conventions: feature flags are `tw_<feature>` or `<feature>_<field>`.
`gui_changePage("name")` from C++ reloads the page and re-reads all vars.

## Adding a new UI feature — checklist

1. **Strings** — add `<string name="myfeature_title">…</string>` to
   `common/languages/en.xml` (and mirror in the other language files you can).
2. **Vars** (if needed) — add placement/flag defaults to `resources/vars.xml`.
3. **Images/shapes** (if needed) — add to `resources/images.xml`; drop the asset under
   `images/` (SVG goes under `SVG/…`, tintable with `tint=`).
4. **Style** (if needed) — add to `resources/styles.xml` (or a theme variant file).
5. **Page** — create `pages/myfeature.xml`, add `<xml name="/twres/pages/myfeature.xml"/>`
   to `ui.xml`. Reuse templates (`base`, `ab`, `navbar*`) and existing row/col vars.
6. **Action handler** (if buttons need logic) — `GUIAction::myfeature_*` in `action.cpp`
   + `ADD_ACTION(...)`. Update `DataManager` values from it.
7. Navigate to it from another page: `<action function="page">myfeature</action>`.

## When you need C++ (not pure XML)

Most UI is pure XML. Reach for C++ only when:
- You need a **custom drawn widget** (e.g. the battery statusbar icon) → subclass
  `GUIObject` in `objects.hpp` + a `.cpp`, add the class to `ProcessNode`'s node map in
  `pages.cpp`, and add the `.cpp` to `gui/Android.bp` `srcs`.
- You need **logic behind a button** → a `GUIAction` handler (above).

## Threading (matters when calling the engine from a GUIAction)

One GUI thread runs the main loop (`gui/gui.cpp`: `select()` on input FIFOs + render/
flip). Actions that mutate the engine run on the **action thread** and are serialized
by `tw_busy`. The `foxcmd`/`twcmd` actions invoke the fox engine from the GUI:
`<action function="foxcmd"/>` → `GUIAction::foxcmd` → `Fox_Fifo::Run_Command()`
(see `docs/fox-protocol.md`). Output is captured via `gui_set_FILE()` → the global
`ors_file` sink in `gui/console.cpp`.

## Build/test notes

- FoxUiEngine is the static lib `libfoxui` (`gui/Android.bp`); conditional GUI sources
  (custom keyboards) are added in `gui/libfoxui_defaults.go`, which reads `TWRP_CUSTOM_KEYBOARD`.
- Theme selection: `TW_THEME` (or `TW_CUSTOM_THEME`, or auto from screen size) in the
  device tree. The chosen `gui/theme/<theme>/` is what ships.
- XML is parsed by rapidxml at page load; errors go to `LOGERR`/`LOGINFO` in the
  recovery kernel log (view via the console page or `adb shell dmesg`/log). A malformed
  page usually shows as a blank/missing page rather than a crash.
- There is no headless XML preview — you test by building the recovery image and
  booting it.

## Related docs

- `../AGENTS.md` — project orientation, build, layout.
- `../docs/fox-protocol.md` — the `fox` engine that `foxcmd` invokes.
