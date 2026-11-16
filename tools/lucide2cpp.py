#!/usr/bin/env python3
"""Turns Lucide SVG icons (https://lucide.dev, ISC licence) into src/lucide_icons.hpp:
compact drawing commands that Dusk draws with cairo (the panel, control centre,
menus) and Dusk Settings uses for its sidebar. Usage:
    lucide2cpp.py LUCIDE_ICONS_DIR OUT.hpp name [name ...]
Commands per element, ';' between elements: M x y | L x y | C x1 y1 x2 y2 x y | Z |
O cx cy r (circle) | E cx cy rx ry (ellipse) | R x y w h r (rounded rect); a leading
'F' fills the element too."""
import math, re, sys, xml.etree.ElementTree as ET

def num(v): return float(v) if v not in (None, '') else 0.0
def f(x):
    s = ('%.3f' % x).rstrip('0').rstrip('.')
    return '0' if s in ('-0', '') else s

def arc_to_cubic(x1, y1, rx, ry, phi, fa, fs, x2, y2):
    if rx == 0 or ry == 0: return [('L', x2, y2)]
    phi = math.radians(phi); c, s = math.cos(phi), math.sin(phi)
    dx, dy = (x1 - x2) / 2, (y1 - y2) / 2
    x1p, y1p = c * dx + s * dy, -s * dx + c * dy
    rx, ry = abs(rx), abs(ry)
    lam = x1p**2 / rx**2 + y1p**2 / ry**2
    if lam > 1: rx *= math.sqrt(lam); ry *= math.sqrt(lam)
    num_ = rx*rx*ry*ry - rx*rx*y1p*y1p - ry*ry*x1p*x1p
    den = rx*rx*y1p*y1p + ry*ry*x1p*x1p
    co = math.sqrt(max(0, num_ / den)) if den else 0
    if fa == fs: co = -co
    cxp, cyp = co * rx * y1p / ry, -co * ry * x1p / rx
    cx = c * cxp - s * cyp + (x1 + x2) / 2
    cy = s * cxp + c * cyp + (y1 + y2) / 2
    def ang(ux, uy, vx, vy):
        a = math.atan2(ux*vy - uy*vx, ux*vx + uy*vy)
        return a
    t1 = ang(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry)
    dt = ang((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry)
    if not fs and dt > 0: dt -= 2 * math.pi
    elif fs and dt < 0: dt += 2 * math.pi
    n = max(1, int(math.ceil(abs(dt) / (math.pi / 2) - 1e-9)))
    d = dt / n; k = 4 / 3 * math.tan(d / 4)
    out = []; t = t1
    for _ in range(n):
        ca, sa, cb, sb = math.cos(t), math.sin(t), math.cos(t + d), math.sin(t + d)
        p1 = (ca - k * sa, sa + k * ca); p2 = (cb + k * sb, sb - k * cb); p3 = (cb, sb)
        pts = []
        for px, py in (p1, p2, p3):
            px, py = px * rx, py * ry
            pts.append((c * px - s * py + cx, s * px + c * py + cy))
        out.append(('C', pts[0][0], pts[0][1], pts[1][0], pts[1][1], pts[2][0], pts[2][1]))
        t += d
    return out

def path_ops(d):
    toks = re.findall(r'[MmLlHhVvCcSsQqTtAaZz]|[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?', d)
    i = 0; cmd = None; x = y = sx = sy = 0.0; ops = []; lc = None; lq = None
    def nxt():
        nonlocal i
        v = float(toks[i]); i += 1; return v
    def flag():
        nonlocal i
        t = toks[i]
        if len(t) > 1 and t[0] in '01':   # packed flags: "a5 5 0 017.07 0" = 0, 1, 7.07
            toks[i] = t[1:]; return int(t[0])
        i += 1; return int(float(t))
    while i < len(toks):
        if re.match(r'[A-Za-z]', toks[i]): cmd = toks[i]; i += 1
        rel = cmd.islower(); C = cmd.upper()
        if C == 'Z':
            ops.append(('Z',)); x, y = sx, sy; lc = lq = None; continue
        if C == 'M':
            nx, ny = nxt(), nxt()
            if rel: nx += x; ny += y
            x, y, sx, sy = nx, ny, nx, ny; ops.append(('M', x, y)); cmd = 'l' if rel else 'L'; lc = lq = None
        elif C == 'L':
            nx, ny = nxt(), nxt()
            if rel: nx += x; ny += y
            x, y = nx, ny; ops.append(('L', x, y)); lc = lq = None
        elif C == 'H':
            nx = nxt(); x = nx + x if rel else nx; ops.append(('L', x, y)); lc = lq = None
        elif C == 'V':
            ny = nxt(); y = ny + y if rel else ny; ops.append(('L', x, y)); lc = lq = None
        elif C in 'CS':
            if C == 'C':
                x1, y1 = nxt(), nxt()
                if rel: x1 += x; y1 += y
            else:
                x1, y1 = (2 * x - lc[0], 2 * y - lc[1]) if lc else (x, y)
            x2, y2, ex, ey = nxt(), nxt(), nxt(), nxt()
            if rel: x2 += x; y2 += y; ex += x; ey += y
            ops.append(('C', x1, y1, x2, y2, ex, ey)); lc = (x2, y2); lq = None; x, y = ex, ey
        elif C in 'QT':
            if C == 'Q':
                qx, qy = nxt(), nxt()
                if rel: qx += x; qy += y
            else:
                qx, qy = (2 * x - lq[0], 2 * y - lq[1]) if lq else (x, y)
            ex, ey = nxt(), nxt()
            if rel: ex += x; ey += y
            ops.append(('C', x + 2/3*(qx-x), y + 2/3*(qy-y), ex + 2/3*(qx-ex), ey + 2/3*(qy-ey), ex, ey))
            lq = (qx, qy); lc = None; x, y = ex, ey
        elif C == 'A':
            rx, ry, phi = nxt(), nxt(), nxt(); fa = flag(); fs = flag(); ex, ey = nxt(), nxt()
            if rel: ex += x; ey += y
            ops += arc_to_cubic(x, y, rx, ry, phi, fa, fs, ex, ey); x, y = ex, ey; lc = lq = None
    return ops

def element(el):
    tag = el.tag.split('}')[-1]; a = el.attrib
    fill = a.get('fill') == 'currentColor'
    if tag == 'path': ops = path_ops(a['d'])
    elif tag == 'circle': ops = [('O', num(a.get('cx')), num(a.get('cy')), num(a.get('r')))]
    elif tag == 'ellipse': ops = [('E', num(a.get('cx')), num(a.get('cy')), num(a.get('rx')), num(a.get('ry')))]
    elif tag == 'rect':
        r = num(a.get('rx', a.get('ry')))
        ops = [('R', num(a.get('x')), num(a.get('y')), num(a.get('width')), num(a.get('height')), r)]
    elif tag == 'line':
        ops = [('M', num(a.get('x1')), num(a.get('y1'))), ('L', num(a.get('x2')), num(a.get('y2')))]
    elif tag in ('polyline', 'polygon'):
        p = [float(v) for v in re.findall(r'[-+]?(?:\d*\.\d+|\d+)', a['points'])]
        ops = [('M', p[0], p[1])] + [('L', p[k], p[k+1]) for k in range(2, len(p), 2)]
        if tag == 'polygon': ops.append(('Z',))
    else: return None
    return ('F ' if fill else '') + ' '.join(o[0] + ''.join(' ' + f(v) for v in o[1:]) for o in ops)

def main():
    src, out, names = sys.argv[1], sys.argv[2], sorted(set(sys.argv[3:]))
    rows = []
    for n in names:
        root = ET.parse(f'{src}/{n}.svg').getroot()
        parts = [p for p in (element(e) for e in root) if p]
        rows.append((n, ';'.join(parts)))
    with open(out, 'w') as o:
        o.write('// Generated by tools/lucide2cpp.py from Lucide (https://lucide.dev), ISC licence:\n'
                '// Copyright (c) Lucide Icons and Contributors (parts derived from Feather, MIT).\n'
                '// 24 x 24 grid, 2 px round lines. Don\'t edit: regenerate.\n#pragma once\n\n'
                'namespace lucide {\n\nstruct Icon {\n\tconst char *name;\n\tconst char *ops;\n};\n\n'
                '// Sorted by name.\ninline constexpr Icon kIcons[] = {\n')
        for n, ops in rows:
            o.write(f'\t{{"{n}", "{ops}"}},\n')
        o.write('};\n\n} // namespace lucide\n')
    print(len(rows), 'icons')

main()
