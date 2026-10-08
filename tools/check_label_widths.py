#!/usr/bin/env python3
"""Measure rendered widths of device strings against the generated LVGL fonts.

The fonts in firmware/src/font_*.c are bitmaps with a fixed glyph set, so a
Spanish string can silently overflow its label or render as blank boxes. This
script decodes each font's glyph advances (adv_w, in 1/16 px) and cmap and then:

  --width FONT TEXT...   pixel width of each text in one font
  --audit FILE...        every lv_label_set_text() literal in the files, with
                         the font its variable uses (best effort) and its width
  --charset FILE...      every string literal that uses a non-ASCII character,
                         checked against the set the regenerated fonts cover
  --max FONT PX TEXT     pass/fail one string against a pixel budget

Exit status is 1 when --audit or --charset reports a problem.
"""
import argparse
import glob
import os
import re
import sys

FONT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "firmware", "src")
PANEL_W = 480          # the display is 480 px wide
WARN_W = 460           # wider than this cannot fit with the outer margin

# The glyph set the regenerated fonts cover beyond ASCII (tools/regen_fonts_spanish.sh)
ALLOWED = set("áéíóúüñÁÉÍÓÚÜÑ¡¿\u00b7\u2026")


def escape_c(lit: str) -> str:
    """Decode the C escapes used in the firmware literals."""
    out, i = [], 0
    while i < len(lit):
        if lit[i] == "\\" and i + 1 < len(lit):
            n = lit[i + 1]
            if n == "n":
                out.append("\n"); i += 2; continue
            if n == "t":
                out.append("\t"); i += 2; continue
            if n == "\\":
                out.append("\\"); i += 2; continue
            if n == '"':
                out.append('"'); i += 2; continue
            if n == "x" and i + 3 < len(lit):
                out.append(chr(int(lit[i + 2:i + 4], 16))); i += 4; continue
            out.append(n); i += 2; continue
        out.append(lit[i]); i += 1
    return "".join(out)


class Font:
    """A generated LVGL 9 bitmap font: glyph advances + codepoint map."""

    def __init__(self, path: str):
        with open(path, "r", encoding="utf-8") as f:
            text = f.read()
        self.name = re.search(r"const lv_font_t (font_\w+) = \{", text).group(1)

        # .adv_w per glyph id, in 1/16 px. The glyph_dsc array is ordered by
        # glyph id (id 0 reserved) — .bitmap_index is the bitmap offset, NOT the id.
        self.adv = [0]
        for m in re.finditer(r"\{\.bitmap_index = (?:\d+|0x[0-9a-fA-F]+), \.adv_w = (0x[0-9a-fA-F]+|\d+),", text):
            self.adv.append(int(m.group(1), 0))

        # cmaps: contiguous FORMAT0_TINY range or SPARSE_TINY list
        self.cp2gid = {}
        for m in re.finditer(
                r"\.range_start = (\d+), \.range_length = (\d+), \.glyph_id_start = (\d+),.*?"
                r"\.unicode_list = (NULL|\w+),.*?\.list_length = (\d+),", text, re.S):
            start, length, gid_start, ulist, ulen = (int(m.group(1)), int(m.group(2)),
                                                     int(m.group(3)), m.group(4), int(m.group(5)))
            if ulist == "NULL":
                for k in range(length):
                    self.cp2gid[start + k] = gid_start + k
            else:
                lst = re.search(r"static const uint16_t %s\[\] = \{([^}]*)\}" % ulist, text)
                for k, val in enumerate(int(v, 0) for v in
                                        re.findall(r"0x[0-9a-fA-F]+|\d+", lst.group(1))):
                    self.cp2gid[start + val] = gid_start + k

    def width_px(self, s: str):
        total = 0.0
        for ch in s:
            if ch == "\n":
                continue
            gid = self.cp2gid.get(ord(ch))
            if gid is None or gid >= len(self.adv):
                return -1
            total += self.adv[gid]
        return total / 16.0

    def missing(self, s: str):
        return sorted({ch for ch in s if ch != "\n" and ord(ch) not in self.cp2gid})


def load_fonts() -> dict:
    fonts = {}
    for path in sorted(glob.glob(os.path.join(FONT_DIR, "font_*.c"))):
        try:
            f = Font(path)
            fonts[f.name] = f
        except Exception as e:
            print(f"warning: {os.path.basename(path)}: {e}", file=sys.stderr)
    return fonts


# Only literal single-argument calls; the non-greedy match is bounded by the
# closing paren so a format call with later arguments can't swallow the file.
SET_TEXT_RE = re.compile(
    r'lv_label_set_text\(\s*([\w.*>-]+)\s*,\s*("(?:[^"\\\n]|\\.)*")\s*\)')
SET_FMT_RE = re.compile(
    r'lv_label_set_text_fmt\(\s*([\w.*>-]+)\s*,\s*("(?:[^"\\\n]|\\.)*")')
VAR_FONT_RE = re.compile(r'lv_obj_set_style_text_font\(\s*([\w.*>-]+)\s*,\s*&?(font_\w+)\s*,')
# Font via the Layout struct: lv_obj_set_style_text_font(lbl, L.<slot>_font, 0).
# The slot itself is filled in compute_layout(), usually once per board-size
# branch (see LAYOUT_FONT_RE below).
L_SLOT_USE_RE = re.compile(r'lv_obj_set_style_text_font\(\s*([\w.*>-]+)\s*,\s*L\.(\w+_font)\s*,')
LAYOUT_FONT_RE = re.compile(r'(?:L\.)?(\w+_font)\s*=\s*&(font_\w+);')


def resolve_fonts(src: str, fonts: dict):
    """label var -> font(s) for one file, following the Layout struct.

    Returns (var_direct, var_slot, slot_fonts):
      var_direct: label variable -> font symbol (style call with &font_x)
      var_slot:   label variable -> L slot name (style call with L.<slot>)
      slot_fonts: L slot name -> set of font symbols assigned in
                  compute_layout() (one per board-size branch, hence a set)

    A label on L.<slot> can therefore render in any font of
    slot_fonts[slot]; the audit width must use the widest of them as the
    upper bound (tiempos_56, styrene_48/28/16, mono_32 all arrive this way in
    ui.cpp — ignoring them made the old "(sin resolver)" fallback, which only
    looked at directly-named fonts, not a real upper bound).
    """
    slot_fonts: dict = {}
    for slot, sym in LAYOUT_FONT_RE.findall(src):
        if sym in fonts:
            slot_fonts.setdefault(slot, set()).add(sym)
    var_direct = {}
    for var_name, sym in VAR_FONT_RE.findall(src):
        if sym in fonts:
            var_direct[var_name] = sym
    var_slot = {}
    for m in L_SLOT_USE_RE.finditer(src):
        var_slot[m.group(1)] = m.group(2)
    return var_direct, var_slot, slot_fonts


def audit(path: str, fonts: dict) -> int:
    with open(path, "r", encoding="utf-8") as f:
        src = f.read()
    var_font, var_slot, slot_fonts = resolve_fonts(src, fonts)
    file_fonts = sorted({s for v in slot_fonts.values() for s in v} | set(var_font.values()))
    problems = 0
    print(f"== {os.path.basename(path)}  ({len(file_fonts)} fonts: {', '.join(file_fonts)})")

    def fonts_for(var):
        """Candidate font symbols for a label variable + display name."""
        if var in var_font:
            return [var_font[var]], var_font[var]
        slot = var_slot.get(var) or var_slot.get(var.lstrip("*"))
        if slot:
            return sorted(slot_fonts.get(slot, ())), f"L.{slot}"
        return [], ""

    def report(line_no, var, syms, shown, text, is_fmt):
        nonlocal problems
        risky = {}
        if not syms:
            # Unresolved variable: the font cannot be proven, so a character that
            # ANY font in the file lacks is a risk worth naming. (U+00B7 and
            # U+2026 live only in the mono and plex cuts, never in the styrene or
            # tiempos ones — a · there would draw LVGL's placeholder box.)
            # file_fonts now unions every font named in compute_layout() with
            # the directly-used ones, so this worst case is a real upper bound.
            if not file_fonts:
                return
            worst = max(fonts[s].width_px(text) for s in file_fonts)
            for s in file_fonts:
                for ch in fonts[s].missing(text):
                    risky.setdefault(ch, []).append(s)
            sym = "(sin resolver)"
        elif len(syms) == 1:
            font = fonts[syms[0]]
            worst = font.width_px(text)
            risky = {ch: [syms[0]] for ch in font.missing(text)}
            sym = shown or syms[0]
        else:
            # L slot with (usually) one font per board-size branch: the upper
            # bound is the widest, and a glyph missing in ANY candidate breaks
            # that board.
            worst = max(fonts[s].width_px(text) for s in syms)
            for s in syms:
                for ch in fonts[s].missing(text):
                    risky.setdefault(ch, []).append(s)
            sym = f"{shown} ({'/'.join(syms)})"
        if risky:
            note = (f"GLIFO NO CUBIERTO {sorted(risky)} — falta en "
                    f"{sorted({f for v in risky.values() for f in v})}")
        elif worst > WARN_W:
            note = "DEMASIADO ANCHO"
        else:
            note = ""
        if note:
            problems += 1
        suffix = "  [fmt: solo la parte literal]" if is_fmt else ""
        print(f"  {line_no:5d}  {var:22s} {sym:18s} {worst:6.0f}px  {text!r} {note}{suffix}")

    for m in SET_TEXT_RE.finditer(src):
        var, lit = m.group(1), m.group(2)
        line_no = src[:m.start()].count("\n") + 1
        text = escape_c(lit[1:-1])
        if not text.strip():
            continue
        report(line_no, var, *fonts_for(var),
               text, False)
    for m in SET_FMT_RE.finditer(src):
        var, lit = m.group(1), m.group(2)
        line_no = src[:m.start()].count("\n") + 1
        # measure the literal skeleton: drop % verbs, they are replaced at runtime
        text = re.sub(r"%[-+ #0-9.*]*(?:d|u|s|f|x|ld|lld|02d|%)", "0",
                      escape_c(lit[1:-1]))
        report(line_no, var, *fonts_for(var),
               text, True)
    return problems


def charset(path: str) -> int:
    """Every non-ASCII char in a *label* literal must be in the font set.

    Literals passed to lv_label_set_text*() are rendered by the bitmap fonts, so
    a character outside the set shows up as a blank box — those are hard errors.
    Any other literal (Serial logs, debug strings) never reaches a font, so an
    unexpected character there is only informational.
    """
    with open(path, "r", encoding="utf-8") as f:
        src = f.read()

    # spans of the literals that actually reach an LVGL label
    label_spans = [m.span(2) for m in SET_TEXT_RE.finditer(src)]
    label_spans += [m.span(2) for m in SET_FMT_RE.finditer(src)]

    hard = warn = 0
    for m in re.finditer(r'"((?:[^"\\\n]|\\.)*)"', src):
        lit = m.group(1)
        if all(ord(c) < 128 for c in lit):
            continue
        line_no = src[:m.start()].count("\n") + 1
        bad = sorted({c for c in lit if ord(c) >= 128 and c not in ALLOWED})
        if not bad:
            continue
        in_label = any(s <= m.start(1) < e for s, e in label_spans)
        if in_label:
            hard += 1
            print(f"  ERROR {os.path.basename(path)}:{line_no}: glifos no cubiertos por los "
                  f"fonts {bad}  {lit!r}")
        else:
            warn += 1
            print(f"  aviso {os.path.basename(path)}:{line_no}: no-ASCII fuera del juego "
                  f"{bad} (no llega a ninguna etiqueta LVGL)  {lit!r}")
    if warn:
        print(f"  ({warn} aviso(s) informativos, no rompen el render)")
    return hard


def main() -> int:
    ap = argparse.ArgumentParser()
    # REMAINDER (not "+"): label texts like '---' or '---%' start with '-'
    # and argparse would otherwise swallow them as unknown flags.
    ap.add_argument("--width", nargs=argparse.REMAINDER, metavar=("FONT", "TEXT"))
    ap.add_argument("--audit", nargs="+", metavar="FILE")
    ap.add_argument("--charset", nargs="+", metavar="FILE")
    ap.add_argument("--max", nargs=3, metavar=("FONT", "PIXELS", "TEXT"))
    args = ap.parse_args()
    fonts = load_fonts()
    if not fonts:
        print("no fonts parsed", file=sys.stderr)
        return 2

    if args.width:
        # REMAINDER keeps dash-leading texts as values (a "--" separator after
        # --width is NOT supported by argparse with REMAINDER — pass texts
        # directly: --width FONT --- ---%).
        rest = [a for a in args.width if a != "--"]
        if not rest:
            print("--width needs FONT TEXT...", file=sys.stderr)
            return 2
        name, texts = rest[0], rest[1:]
        if name not in fonts:
            print(f"unknown font {name}; have: {', '.join(sorted(fonts))}", file=sys.stderr)
            return 2
        for t in texts:
            f = fonts[name]
            w = f.width_px(t)
            miss = f.missing(t)
            print(f"{name} {w:7.1f}px  {t!r}" + (f"  MISSING {miss}" if miss else ""))
    if args.max:
        name, px, t = args.max
        w = fonts[name].width_px(t)
        ok = 0 <= w <= float(px)
        print(f"{'OK ' if ok else 'OVER'} {w:.1f}px / {px}px  {t!r}")
        return 0 if ok else 1
    if args.charset:
        total = sum(charset(p) for p in args.charset)
        print(f"--- {total} literal(es) fuera del juego de glifos")
        return 1 if total else 0
    if args.audit:
        total = sum(audit(p, fonts) for p in args.audit)
        print(f"--- {total} problema(s) de ancho/glifos")
        return 1 if total else 0
    acted = bool(args.width or args.max or args.charset or args.audit)
    if not acted:
        ap.print_help()
        return 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
