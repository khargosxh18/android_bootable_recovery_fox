#!/usr/bin/env python3
# FoxUiEngine headless linter / preview.
#
# Parses an OrangeFox theme on the host (no device needed) and:
#   * lints static references: style=, resource=/res=, and {@string} that point
#     at names which aren't defined anywhere in the theme;
#   * optionally renders a rough placement-box SVG preview of one page so you
#     can eyeball layout without building+booting a recovery image.
#
# It deliberately does NOT try to validate %runtime_vars% (tw_*, fox_*, ...),
# which live in DataManager at runtime, not in the theme.
#
# Usage:
#   foxui_lint.py [THEME_DIR]                      # lint the whole theme
#   foxui_lint.py [THEME_DIR] --svg PAGE --width 1080 --height 2400 > page.svg
#
# THEME_DIR defaults to ../theme/portrait_hdpi relative to this script.

import os
import re
import sys
import glob
import argparse

NAME_RE = re.compile(r'name="([^"]+)"')


def collect(defs, path, tag):
    """Collect name="X" from <tag ...> definitions in a file."""
    if not os.path.isfile(path):
        return
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            for m in re.finditer(r'<%s\b[^>]*?name="([^"]+)"' % tag, line):
                defs.add(m.group(1))


def collect_defs(theme):
    res = os.path.join(theme, "resources")
    variables, styles, strings, images = set(), set(), set(), set()

    collect(variables, os.path.join(res, "vars.xml"), "variable")

    # styles can live in resources/styles.xml and themes/styles/*.xml
    for p in [os.path.join(res, "styles.xml")] + glob.glob(os.path.join(theme, "themes", "**", "*.xml"), recursive=True):
        collect(styles, p, "style")

    for tag in ("image", "shape", "animation"):
        collect(images, os.path.join(res, "images.xml"), tag)

    # strings from the common english file
    for p in glob.glob(os.path.join(theme, "..", "common", "languages", "en.xml")):
        collect(strings, p, "string")

    return variables, styles, strings, images


def lint(theme, check_images=False):
    variables, styles, strings, images = collect_defs(theme)
    pages = glob.glob(os.path.join(theme, "pages", "**", "*.xml"), recursive=True)
    issues = 0
    # Icons frequently resolve by filename convention rather than an images.xml
    # entry, so the image check is opt-in (--check-images) to stay low-noise.

    style_re = re.compile(r'\bstyle="([^"]+)"')
    res_re = re.compile(r'\b(?:resource|res)="([^"]+)"')
    str_re = re.compile(r'\{@([A-Za-z0-9_]+)')

    for page in sorted(pages):
        with open(page, encoding="utf-8", errors="replace") as f:
            for n, line in enumerate(f, 1):
                for m in style_re.finditer(line):
                    name = m.group(1)
                    if "%" in name:
                        continue
                    if name not in styles:
                        print("%s:%d: unknown style \"%s\"" % (page, n, name))
                        issues += 1
                if check_images:
                    for m in res_re.finditer(line):
                        name = m.group(1)
                        if "%" in name or not name:
                            continue
                        if name not in images:
                            print("%s:%d: unknown image/shape/animation \"%s\"" % (page, n, name))
                            issues += 1
                for m in str_re.finditer(line):
                    name = m.group(1)
                    if name not in strings:
                        print("%s:%d: unknown string id \"%s\"" % (page, n, name))
                        issues += 1

    print("\n%d issue(s) across %d page file(s); defs: %d vars, %d styles, %d strings, %d images"
          % (issues, len(pages), len(variables), len(styles), len(strings), len(images)),
          file=sys.stderr)
    return 1 if issues else 0


# ---- rough placement preview --------------------------------------------------

def load_var_values(theme):
    """Read vars.xml into name->expression, for the best-effort evaluator."""
    vals = {}
    p = os.path.join(theme, "resources", "vars.xml")
    if not os.path.isfile(p):
        return vals
    with open(p, encoding="utf-8", errors="replace") as f:
        text = f.read()
    for m in re.finditer(r'<variable\s+name="([^"]+)"\s+value="([^"]*)"', text):
        vals[m.group(1)] = m.group(2)
    return vals


def eval_expr(expr, vals, width, height, seen=None):
    """Best-effort: resolve %refs% and evaluate simple arithmetic. None on failure."""
    if expr is None:
        return None
    seen = seen or set()
    s = str(expr)
    # framebuffer magic values
    s = s.replace("%screen_w%", str(width)).replace("%screen_h%", str(height))

    def repl(m):
        name = m.group(1)
        if name in seen or name not in vals:
            return "0"
        return "(%s)" % (eval_expr(vals[name], vals, width, height, seen | {name}) or 0)

    s = re.sub(r'%([A-Za-z0-9_]+)%', repl, s)
    if not re.fullmatch(r'[-+*/() 0-9]+', s or ""):
        return None
    try:
        return int(eval(s))
    except Exception:
        return None


def make_svg(theme, page_name, width, height):
    vals = load_var_values(theme)
    path = None
    for p in glob.glob(os.path.join(theme, "pages", "**", "*.xml"), recursive=True):
        with open(p, encoding="utf-8", errors="replace") as f:
            if re.search(r'<page\s+name="%s"' % re.escape(page_name), f.read()):
                path = p
                break
    if not path:
        print("page '%s' not found" % page_name, file=sys.stderr)
        return 1

    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()

    boxes = []
    for m in re.finditer(r'<placement\b([^>]*)/?>', text):
        attrs = dict(re.findall(r'(\w+)="([^"]*)"', m.group(1)))
        x = eval_expr(attrs.get("x", "0"), vals, width, height)
        y = eval_expr(attrs.get("y", "0"), vals, width, height)
        w = eval_expr(attrs.get("w", "40"), vals, width, height)
        h = eval_expr(attrs.get("h", "40"), vals, width, height)
        if None in (x, y):
            continue
        boxes.append((x, y, w or 40, h or 40))

    out = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">'
           % (width, height, width, height)]
    out.append('<rect width="%d" height="%d" fill="#111"/>' % (width, height))
    for (x, y, w, h) in boxes:
        out.append('<rect x="%d" y="%d" width="%d" height="%d" fill="none" stroke="#4af" stroke-width="2"/>'
                   % (x, y, w, h))
    out.append("</svg>")
    print("\n".join(out))
    print("rendered %d placement box(es) for page '%s'" % (len(boxes), page_name), file=sys.stderr)
    return 0


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description="FoxUiEngine theme linter / preview")
    ap.add_argument("theme", nargs="?", default=os.path.join(here, "..", "theme", "portrait_hdpi"))
    ap.add_argument("--svg", metavar="PAGE", help="render a placement-box SVG for PAGE to stdout")
    ap.add_argument("--check-images", action="store_true",
                    help="also flag resource=/res= names not in images.xml (noisy: icons often resolve by filename)")
    ap.add_argument("--width", type=int, default=1080)
    ap.add_argument("--height", type=int, default=2400)
    args = ap.parse_args()

    theme = os.path.abspath(args.theme)
    if not os.path.isdir(theme):
        print("theme dir not found: %s" % theme, file=sys.stderr)
        return 2

    if args.svg:
        return make_svg(theme, args.svg, args.width, args.height)
    return lint(theme, check_images=args.check_images)


if __name__ == "__main__":
    sys.exit(main())
