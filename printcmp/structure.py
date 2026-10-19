"""Tabeller ur PDF-geometrin (ingen OCR, ingen bildanalys).

Med linjer: tunna horisontella/vertikala linjer (objmap "Rule") som staplas över samma
x-intervall bildar rader; vertikala linjer – eller vita gångar mellan orden – bildar kolumner.

Utan linjer: textrader (ord på samma baslinje) med jämnt radavstånd där en eller flera
vertikala vita gångar går genom alla rader, och där första och sista raden har innehåll på
båda sidor om gången.

Resultatet används av klassningen: en skillnad får tabell, rad, kolumn och cellens text,
eller "tabellinje mellan rad r och r+1".
"""
from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np

from .objmap import ObjEntry


@dataclass
class Table:
    id: int
    kind: str                         # ruled_grid | ruled_horizontal | unruled
    bbox: tuple                       # x0, y0, x1, y1 (ref px)
    row_bounds: list                  # y-gränser, len = rader + 1
    col_bounds: list                  # x-gränser, len = kolumner + 1
    cells: dict = field(default_factory=dict)       # (r, c) -> text
    rules: list = field(default_factory=list)       # {"bbox", "orient", "index", "position"}

    @property
    def n_rows(self):
        return len(self.row_bounds) - 1

    @property
    def n_cols(self):
        return len(self.col_bounds) - 1

    def to_json(self, ppm: float) -> dict:
        return {"id": self.id, "kind": self.kind,
                "bbox_mm": [round(self.bbox[0] / ppm, 2), round(self.bbox[1] / ppm, 2),
                            round((self.bbox[2] - self.bbox[0]) / ppm, 2), round((self.bbox[3] - self.bbox[1]) / ppm, 2)],
                "rows": self.n_rows, "cols": self.n_cols, "rules": len(self.rules),
                "cells": [[self.cells.get((r, c), "") for c in range(self.n_cols)] for r in range(self.n_rows)]}


def _cluster(vals, tol):
    vals = sorted(vals)
    out, cur = [], [vals[0]] if vals else []
    for v in vals[1:]:
        if v - cur[-1] <= tol:
            cur.append(v)
        else:
            out.append(float(np.mean(cur)))
            cur = [v]
    if cur:
        out.append(float(np.mean(cur)))
    return out


def _center(b):
    return ((b[0] + b[2]) / 2, (b[1] + b[3]) / 2)


def text_lines(words: list[ObjEntry]) -> list[list[ObjEntry]]:
    """Ord -> rader (gemensam baslinje), sorterade uppifrån och vänster till höger."""
    ws = sorted(words, key=lambda o: _center(o.bbox)[1])
    lines: list[list[ObjEntry]] = []
    for w in ws:
        cy = _center(w.bbox)[1]
        h = w.bbox[3] - w.bbox[1]
        placed = False
        for ln in lines[-3:]:
            lcy = np.median([_center(o.bbox)[1] for o in ln])
            lh = np.median([o.bbox[3] - o.bbox[1] for o in ln])
            if abs(cy - lcy) <= 0.35 * max(h, lh):
                ln.append(w)
                placed = True
                break
        if not placed:
            lines.append([w])
    for ln in lines:
        ln.sort(key=lambda o: o.bbox[0])
    lines.sort(key=lambda ln: np.median([_center(o.bbox)[1] for o in ln]))
    return lines


def _separators(rows: list[list[ObjEntry]], x0: float, x1: float, gap_min: float) -> list[tuple[float, float]]:
    """Vita vertikala gångar som går genom alla rader, mellan x0 och x1."""
    n = int(np.ceil(x1 - x0)) + 1
    if n <= 2:
        return []
    occ = np.zeros(n, bool)
    for ln in rows:
        for w in ln:
            a = int(max(0, np.floor(w.bbox[0] - x0)))
            b = int(min(n, np.ceil(w.bbox[2] - x0)))
            occ[a:b] = True
    seps, i = [], 0
    filled = np.nonzero(occ)[0]
    if len(filled) == 0:
        return []
    lo, hi = filled[0], filled[-1]
    i = lo
    while i <= hi:
        if not occ[i]:
            j = i
            while j <= hi and not occ[j]:
                j += 1
            if j - i >= gap_min:
                seps.append((x0 + i, x0 + j))
            i = j
        else:
            i += 1
    return seps


def _both_sides(ln: list[ObjEntry], sep) -> bool:
    return any(w.bbox[2] <= sep[0] + 1 for w in ln) and any(w.bbox[0] >= sep[1] - 1 for w in ln)


def _fill_cells(t: Table, words: list[ObjEntry]):
    for w in words:
        cx, cy = _center(w.bbox)
        if not (t.bbox[0] <= cx <= t.bbox[2] and t.bbox[1] <= cy <= t.bbox[3]):
            continue
        r = int(np.searchsorted(t.row_bounds, cy) - 1)
        c = int(np.searchsorted(t.col_bounds, cx) - 1)
        r = min(max(r, 0), t.n_rows - 1)
        c = min(max(c, 0), t.n_cols - 1)
        prev = t.cells.get((r, c), "")
        t.cells[(r, c)] = (prev + " " + w.text).strip()


def detect(objs: list[ObjEntry], ppm: float) -> list[Table]:
    words = [o for o in objs if o.kind == "Text"]
    rules = [o for o in objs if o.kind == "Rule"]
    hr = [r for r in rules if (r.bbox[2] - r.bbox[0]) >= (r.bbox[3] - r.bbox[1])]
    vr = [r for r in rules if (r.bbox[2] - r.bbox[0]) < (r.bbox[3] - r.bbox[1])]
    tables: list[Table] = []
    used_h = set()

    # ---------------- med linjer ----------------
    order = sorted(range(len(hr)), key=lambda i: _center(hr[i].bbox)[1])
    for i in order:
        if i in used_h:
            continue
        a = hr[i].bbox
        stack = [i]
        for j in order:
            if j in used_h or j == i:
                continue
            b = hr[j].bbox
            ov = min(a[2], b[2]) - max(a[0], b[0])
            if ov >= 0.6 * min(a[2] - a[0], b[2] - b[0]):
                stack.append(j)
        ys = _cluster([_center(hr[k].bbox)[1] for k in stack], 0.4 * ppm)
        # dela stacken där avståndet mellan linjer blir orimligt stort (> 15 mm)
        groups, cur = [], [ys[0]]
        for y in ys[1:]:
            if y - cur[-1] > 15 * ppm:
                groups.append(cur)
                cur = [y]
            else:
                cur.append(y)
        groups.append(cur)
        for g in groups:
            members = [k for k in stack if g[0] - 0.5 * ppm <= _center(hr[k].bbox)[1] <= g[-1] + 0.5 * ppm]
            if not members:
                continue
            x0 = float(np.median([hr[k].bbox[0] for k in members]))
            x1 = float(np.median([hr[k].bbox[2] for k in members]))
            y0, y1 = g[0], g[-1]
            vin = [v for v in vr if x0 - ppm <= _center(v.bbox)[0] <= x1 + ppm
                   and min(v.bbox[3], y1) - max(v.bbox[1], y0) >= 0.5 * (y1 - y0)]
            vxs = _cluster([_center(v.bbox)[0] for v in vin], 0.4 * ppm)
            inner_v = [x for x in vxs if x0 + 0.5 * ppm < x < x1 - 0.5 * ppm]
            if len(g) < 3 and not (len(g) >= 2 and len(vxs) >= 2):
                continue
            row_bounds = list(g)
            if inner_v:
                col_bounds = [x0] + inner_v + [x1]
                kind = "ruled_grid"
            else:
                inside = [w for w in words if x0 <= _center(w.bbox)[0] <= x1 and y0 <= _center(w.bbox)[1] <= y1]
                rows_w = text_lines(inside)
                gap_min = max(1.5 * ppm, float(np.median([w.bbox[3] - w.bbox[1] for w in inside])) if inside else ppm)
                seps = _separators(rows_w, x0, x1, gap_min) if rows_w else []
                col_bounds = [x0] + [(s0 + s1) / 2 for s0, s1 in seps] + [x1]
                kind = "ruled_horizontal" if not vxs else "ruled_grid"
            if (len(row_bounds) - 1) * (len(col_bounds) - 1) < 2:
                continue
            t = Table(len(tables) + 1, kind, (x0, y0, x1, y1), row_bounds, col_bounds)
            for k in members:
                y = _center(hr[k].bbox)[1]
                t.rules.append({"bbox": hr[k].bbox, "orient": "h", "obj": hr[k].id,
                                "position": int(np.argmin([abs(y - rb) for rb in row_bounds]))})
            for v in vin:
                x = _center(v.bbox)[0]
                t.rules.append({"bbox": v.bbox, "orient": "v", "obj": v.id,
                                "position": int(np.argmin([abs(x - cb) for cb in col_bounds]))})
            _fill_cells(t, words)
            tables.append(t)
            used_h.update(members)

    # ---------------- utan linjer ----------------
    def in_table(w):
        cx, cy = _center(w.bbox)
        return any(t.bbox[0] - ppm <= cx <= t.bbox[2] + ppm and t.bbox[1] - ppm <= cy <= t.bbox[3] + ppm
                   for t in tables)

    free = [w for w in words if not in_table(w)]
    lines = text_lines(free)
    cys = [float(np.median([_center(o.bbox)[1] for o in ln])) for ln in lines]
    hs = [float(np.median([o.bbox[3] - o.bbox[1] for o in ln])) for ln in lines]
    i = 0
    while i < len(lines) - 2:
        best = None
        for j in range(i + 2, len(lines)):
            seg = lines[i:j + 1]
            sp = np.diff(cys[i:j + 1])
            hm = float(np.median(hs[i:j + 1]))
            if sp.max() > 2.2 * hm or sp.min() < 0.8 * hm or (sp.max() - sp.min()) > 0.35 * float(np.median(sp)):
                break
            if max(hs[i:j + 1]) > 1.6 * min(hs[i:j + 1]):
                break
            x0 = min(w.bbox[0] for ln in seg for w in ln)
            x1 = max(w.bbox[2] for ln in seg for w in ln)
            seps = _separators(seg, x0, x1, max(1.5 * ppm, hm))
            seps = [s for s in seps if _both_sides(seg[0], s) and _both_sides(seg[-1], s)
                    and sum(_both_sides(ln, s) for ln in seg) >= len(seg) - 1]
            if not seps:
                break
            best = (j, seps, x0, x1, float(np.median(sp)))
        if best is None:
            i += 1
            continue
        j, seps, x0, x1, step = best
        rb = [cys[i] - step / 2] + [(cys[k] + cys[k + 1]) / 2 for k in range(i, j)] + [cys[j] + step / 2]
        cb = [x0 - 0.5 * ppm] + [(s0 + s1) / 2 for s0, s1 in seps] + [x1 + 0.5 * ppm]
        t = Table(len(tables) + 1, "unruled", (cb[0], rb[0], cb[-1], rb[-1]), rb, cb)
        _fill_cells(t, free)
        tables.append(t)
        i = j + 1
    return tables


def locate(tables: list[Table], box, ppm: float, blob_mask=None) -> dict | None:
    """Var i en tabell ligger en skillnad? box = (x0, y0, x1, y1) i ref px."""
    cx, cy = _center(box)
    for t in tables:
        if not (t.bbox[0] - ppm <= cx <= t.bbox[2] + ppm and t.bbox[1] - ppm <= cy <= t.bbox[3] + ppm):
            continue
        # linje? (större delen av skillnadens ruta ligger inom en linjes ruta)
        bw, bh = box[2] - box[0], box[3] - box[1]
        for rl in t.rules:
            rbx = rl["bbox"]
            pad = 0.3 * ppm
            ix = max(0.0, min(box[2], rbx[2] + pad) - max(box[0], rbx[0] - pad))
            iy = max(0.0, min(box[3], rbx[3] + pad) - max(box[1], rbx[1] - pad))
            thin = min(bw, bh) <= 0.8 * ppm
            if thin and ix * iy >= 0.6 * max(bw * bh, 1.0):
                if rl["orient"] == "h":
                    p = rl["position"]
                    return {"table": t.id, "element": "rule", "orient": "h", "position": p,
                            "between_rows": [p - 1, p] if 0 < p < t.n_rows else None}
                return {"table": t.id, "element": "rule", "orient": "v", "position": rl["position"]}
        r = int(np.searchsorted(t.row_bounds, cy) - 1)
        c = int(np.searchsorted(t.col_bounds, cx) - 1)
        r = min(max(r, 0), t.n_rows - 1)
        c = min(max(c, 0), t.n_cols - 1)
        # en skillnad som spänner över flera kolumner gäller hela raden
        c0 = int(np.searchsorted(t.col_bounds, box[0] + 1) - 1)
        c1 = int(np.searchsorted(t.col_bounds, box[2] - 1) - 1)
        info = {"table": t.id, "element": "cell", "row": r, "col": c if c0 == c1 else None,
                "row_label": t.cells.get((r, 0), ""), "cell_text": t.cells.get((r, c), "") if c0 == c1 else None}
        return info
    return None
