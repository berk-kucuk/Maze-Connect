#!/usr/bin/env python3
"""Maze Connect icon set, generated from one grid.

Every variant -- desktop app icon, tray, panel mark, Android adaptive layers,
notification glyph, store icon -- is drawn on the same 1024 x 1024 grid that
the rest of the Maze family uses:

  * tile    full canvas, corner radius 224 (21.875 %), black -- Maze AI's tile
            (112 on its 512 grid) and Qlam's radius 224 on 1024.
  * content the rings match Maze AI's (outer 212..812, inner 336..688), so the
            mark appears the same size as its neighbours in the tray.
  * stroke  44 for the detailed mark (Maze AI's 22 @ 512), 88 for the small
            mark used at 16-32 px.

The artwork is the Maze Linux nested-square maze opened on both sides, with a
single path running straight through it: two endpoints, one route.

Usage: gen_icons.py OUT_DIR   (writes SVG masters; render.sh makes the PNGs)
"""

import sys
from pathlib import Path

GRID = 1024
TILE_RX = 224
WHITE = "#f4f4f6"


def _rect(x0, y0, x1, y1, fill):
    return f'<rect x="{x0}" y="{y0}" width="{x1 - x0}" height="{y1 - y0}" fill="{fill}"/>'


def mark_large(color=WHITE, glow=True):
    """The detailed mark, for 48 px and up."""
    s = 44
    st = (f'fill="none" stroke="{color}" stroke-width="{s}" '
          'stroke-linecap="square" stroke-linejoin="miter"')
    parts = []
    # Outer ring, opened left and right.
    parts.append(f'<path {st} d="M212 392 V212 H812 V392 M212 632 V812 H812 V632"/>')
    # Inner ring, opened the same way.
    parts.append(f'<path {st} d="M336 436 V336 H688 V436 M336 588 V688 H688 V588"/>')
    if glow:
        parts.append('<circle cx="512" cy="512" r="210" fill="url(#mc-glow)"/>')
    # The route: endpoint, path, the hollow centre of the maze, path, endpoint.
    parts.append(f'<path fill="none" stroke="{color}" stroke-width="{s}" '
                 'd="M252 512 H424 M600 512 H772"/>')
    parts.append(f'<rect x="444" y="444" width="136" height="136" fill="none" '
                 f'stroke="{color}" stroke-width="40"/>')
    parts.append(_rect(172, 472, 252, 552, color))
    parts.append(_rect(772, 472, 852, 552, color))
    return "".join(parts)


def mark_small(color=WHITE):
    """The reduced mark, for 16-32 px: no inner ring, doubled weight."""
    s = 88
    st = (f'fill="none" stroke="{color}" stroke-width="{s}" '
          'stroke-linecap="square" stroke-linejoin="miter"')
    return "".join([
        f'<path {st} d="M256 340 V256 H768 V340 M256 684 V768 H768 V684"/>',
        f'<path fill="none" stroke="{color}" stroke-width="{s}" d="M320 512 H704"/>',
        _rect(432, 432, 592, 592, color),
        _rect(192, 448, 320, 576, color),
        _rect(704, 448, 832, 576, color),
    ])


GLOW_DEF = ('<defs><radialGradient id="mc-glow" cx="0.5" cy="0.5" r="0.5">'
            '<stop offset="0" stop-color="#fff" stop-opacity="0.38"/>'
            '<stop offset="0.5" stop-color="#fff" stop-opacity="0.08"/>'
            '<stop offset="1" stop-color="#fff" stop-opacity="0"/>'
            '</radialGradient></defs>')


def svg(body, defs=""):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {GRID} {GRID}" '
            f'width="{GRID}" height="{GRID}"><title>Maze Connect</title>{defs}{body}</svg>\n')


def tile(rx=TILE_RX):
    return f'<rect width="{GRID}" height="{GRID}" rx="{rx}" fill="#000"/>'


def main(out: Path):
    out.mkdir(parents=True, exist_ok=True)
    files = {
        # App icon, 48 px and up (and the hicolor scalable entry).
        "maze-connect.svg": svg(tile() + mark_large(), GLOW_DEF),
        # App icon, 16-32 px: tray, task switcher, small launchers.
        "maze-connect-small.svg": svg(tile() + mark_small()),
        # Ground-free mark for the sidebar and watermark (drawn on a panel).
        "maze-connect-mark.svg": svg(mark_large(), GLOW_DEF),
        # Square, unrounded: Play applies its own mask to the store icon.
        "maze-connect-store.svg": svg(tile(rx=0) + mark_large(), GLOW_DEF),
        # Round legacy launcher icon.
        "maze-connect-round.svg": svg(
            f'<circle cx="512" cy="512" r="512" fill="#000"/>' + mark_large(), GLOW_DEF),
    }
    for name, text in files.items():
        (out / name).write_text(text)


if __name__ == "__main__":
    main(Path(sys.argv[1] if len(sys.argv) > 1 else "."))
