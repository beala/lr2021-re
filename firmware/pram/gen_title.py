#!/usr/bin/env python3
"""Make ray_title.h: the raycaster's title screen as a vector display list (ray.c op 2).

A blocky take on the DOOM logo (a bevel shadow under each letter), "LR2021" underneath in a small
stroke font. Drawn in screen units (x 0-80, y -31..31, up is +); stored as ray.c's display list
points: x in half units (0-160), then (y in half units + 64) | pen << 7, pen = a move (beam off)
to the polyline's first point.
"""
import os


def poly(*pts, closed=True):
    pts = list(pts)
    return pts + [pts[0]] if closed else pts


D_OUT = poly((4, 16), (16, 16), (21, 11), (21, -12), (16, -17), (4, -17))
D_IN = poly((9, 10), (14, 10), (16, 8), (16, -9), (14, -11), (9, -11))
O_OUT = poly((26, 14), (34, 14), (37, 11), (37, -12), (34, -15), (26, -15), (23, -12), (23, 11))
O_IN = poly((28, 8), (32, 8), (32, -9), (28, -9))
M_OUT = poly((57, 16), (63, 16), (67, 8), (71, 16), (77, 16), (77, -17), (72, -17), (72, 4), (68, -4),
             (66, -4), (62, 4), (62, -17), (57, -17))


def shift(p, dx, dy=0):
    return [(x + dx, y + dy) for x, y in p]


LETTERS = [D_OUT, D_IN, O_OUT, O_IN, shift(O_OUT, 17), shift(O_IN, 17), M_OUT]
# the bevel: each letter's bottom edge again, a little lower and to the right
BEVEL = [[(5, -19), (17, -19), (22, -14), (22, 9)],
         [(24, -14), (27, -17), (35, -17), (38, -14), (38, 9)],
         [(41, -14), (44, -17), (52, -17), (55, -14), (55, 9)],
         [(58, -19), (63, -19), (63, -6)], [(73, -19), (78, -19), (78, 14)]]


def glyphs(text, x0, y0, w=3, h=7, gap=5):
    """'LR2021' in strokes, bottom-left at (x0, y0)."""
    g = {'L': [[(0, h), (0, 0), (w, 0)]],
         'R': [[(0, 0), (0, h), (w, h), (w, 4), (0, 4), (w, 0)]],
         '2': [[(0, h), (w, h), (w, 4), (0, 3), (0, 0), (w, 0)]],
         '0': [[(0, 0), (0, h), (w, h), (w, 0), (0, 0)]],
         '1': [[(w // 2 - 1, h - 1), (w // 2, h), (w // 2, 0)]]}
    out = []
    for i, c in enumerate(text):
        for s in g[c]:
            out.append([(x0 + i * gap + x, y0 + y) for x, y in s])
    return out


def main():
    lines = LETTERS + BEVEL + glyphs('LR2021', 26, -29)
    pts = []
    for ln in lines:
        for k, (x, y) in enumerate(ln):
            assert 0 <= x <= 80 and -31 <= y <= 31, (x, y)
            pts.append((2 * x, (2 * y + 64) | (0 if k else 0x80)))
    assert len(pts) <= 150, len(pts)
    body = ',\n'.join('  ' + ', '.join(f'{a}, {b}' for a, b in pts[i:i + 8]) for i in range(0, len(pts), 8))
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'ray_title.h')
    open(out, 'w').write(f"""// The raycaster's title screen: {len(pts)} beam points in ray.c's display list format.
// Made by gen_title.py; edit that, not this.
#define TITLE_N {len(pts)}
static const unsigned char TITLE[2 * TITLE_N] = {{
{body}
}};
""")
    print(f'{out}: {len(pts)} points')


if __name__ == '__main__':
    main()
