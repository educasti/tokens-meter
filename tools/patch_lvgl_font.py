#!/usr/bin/env python3
"""Patch a raw lv_font_conv (v1.5.3) output into the repo's LVGL 9 format.

lv_font_conv v1.5.3 still emits LVGL 8 guards (``#if LVGL_VERSION_MAJOR >= 8``
blocks, the ``.cache`` field) and names both the include guard and the public
font symbol after the output filename.  The committed ``firmware/src/font_*.c``
files are patched by hand for LVGL 9 — this script makes that patch repeatable.

Usage:
    patch_lvgl_font.py <raw.c> <dst.c> <symbol>

``symbol`` is the public font name the firmware expects (e.g. ``font_styrene_12``).

The transformations are literal block replacements against the deterministic
output of lv_font_conv v1.5.3 (the version pinned in tools/regen_fonts_spanish.sh),
so a regeneration with the same flags reproduces the committed files byte for byte.
"""
import re
import sys

# The raw header carries the generating machine's absolute paths; canonicalise it
# so regenerated files diff cleanly.
OPTS_RE = re.compile(r"^ \* Opts: .*$", re.M)
# lv_font_conv derives the include guard from the -o filename, e.g. "fontgen-AB12CD".
GUARD_RE = re.compile(r"#ifndef\s+[A-Za-z0-9_-]+\n#define\s+[A-Za-z0-9_-]+\s+1\n#endif")
IF_GUARD_RE = re.compile(r"^#if\s+[A-Za-z0-9_-]+\s*$", re.M)
ENDIF_GUARD_RE = re.compile(r"#endif\s*/\*#if\s+[A-Za-z0-9_-]+\s*\*/")


def patch(text: str, symbol: str) -> str:
    guard = symbol.upper()  # FONT_STYRENE_12

    text = OPTS_RE.sub(f" * Opts: tools/regen_fonts_spanish.sh — {symbol} (LVGL 9, +Spanish glyphs)",
                       text, count=1)

    # 1. include guard + outer #if: derived from the -o filename → the font symbol
    text = GUARD_RE.sub(f"#ifndef {guard}\n#define {guard} 1\n#endif", text, count=1)
    text = IF_GUARD_RE.sub(f"#if {guard}", text, count=1)
    # the committed files carry a blank line after the outer #if
    text = text.replace(f"#if {guard}\n/*", f"#if {guard}\n\n/*")
    text = ENDIF_GUARD_RE.sub(f"#endif /*#if {guard}*/", text)

    # 2. the LVGL 8 glyph cache: keep the (now empty) #if wrapper, drop the object
    text = text.replace("static  lv_font_fmt_txt_glyph_cache_t cache;\n", "")

    # 3/4. unwrap the font_dsc version guards and drop .cache (and its comma)
    text = text.replace(
        "#if LVGL_VERSION_MAJOR >= 8\n"
        "static const lv_font_fmt_txt_dsc_t font_dsc = {\n"
        "#else\n"
        "static lv_font_fmt_txt_dsc_t font_dsc = {\n"
        "#endif\n",
        "static const lv_font_fmt_txt_dsc_t font_dsc = {\n")
    text = text.replace(
        "    .bitmap_format = 0,\n"
        "#if LVGL_VERSION_MAJOR == 8\n"
        "    .cache = &cache\n"
        "#endif\n"
        "};",
        "    .bitmap_format = 0\n};")

    # 5. public font: unwrap the version guards, rename the symbol
    text = re.sub(
        r"#if LVGL_VERSION_MAJOR >= 8\n"
        r"const lv_font_t [A-Za-z0-9_-]+ = \{\n"
        r"#else\n"
        r"lv_font_t [A-Za-z0-9_-]+ = \{\n"
        r"#endif\n",
        f"const lv_font_t {symbol} = {{\n", text)

    # 6. strip the generator's inline comments (must run before 7, which rewrites
    #    the .get_glyph_bitmap line)
    text = text.replace("lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/",
                        "lv_font_get_glyph_dsc_fmt_txt,")
    text = text.replace("lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/",
                        "lv_font_get_bitmap_fmt_txt,")
    text = text.replace(".line_height = 14,          /*The maximum line height required by the font*/",
                        ".line_height = 14,")
    text = text.replace(".base_line = 3,             /*Baseline measured from the bottom of the line*/",
                        ".base_line = 3,")
    text = text.replace(".dsc = &font_dsc,          /*The custom font data. Will be accessed by"
                        " `get_glyph_bitmap/dsc` */", ".dsc = &font_dsc,")

    # 7. add the LVGL 9-only field next to the getters, and drop the remaining
    #    version guards around fields LVGL 9 always has (order matters: the
    #    guard removal must run before the field insertion that follows .subpx)
    text = text.replace(
        "    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,\n",
        "    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,\n"
        "    .release_glyph = NULL,\n")
    text = text.replace(
        "#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)\n"
        "    .subpx = LV_FONT_SUBPX_NONE,\n"
        "#endif\n",
        "    .subpx = LV_FONT_SUBPX_NONE,\n")
    text = text.replace(
        "#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8\n"
        "    .underline_position = 0,\n"
        "    .underline_thickness = 1,\n"
        "#endif\n",
        "    .underline_position = 0,\n"
        "    .underline_thickness = 1,\n")
    text = text.replace(
        "#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9\n"
        "    .fallback = NULL,\n"
        "#endif\n",
        "    .fallback = NULL,\n")
    # kerning/static_bitmap sit after .subpx in the committed files
    text = text.replace(
        "    .subpx = LV_FONT_SUBPX_NONE,\n",
        "    .subpx = LV_FONT_SUBPX_NONE,\n"
        "    .kerning = 0,\n"
        "    .static_bitmap = 0,\n")

    return text


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    src, dst, symbol = sys.argv[1:4]
    with open(src, "r", encoding="utf-8") as f:
        text = f.read()
    with open(dst, "w", encoding="utf-8") as f:
        f.write(patch(text, symbol))
    return 0


if __name__ == "__main__":
    sys.exit(main())
