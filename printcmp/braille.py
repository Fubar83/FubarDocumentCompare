"""Punktskrift (braille): läsning ur PDF, cellnät, översättning och kontroll.

Källor i PDF:en (i prioritetsordning):
  1. Dekorfärg för prägling (t.ex. "Braille", "Punktskrift", "Emboss") – punkter som cirklar.
  2. Unicode-punktskrift (U+2800–U+28FF) i textlagret – punkter härleds ur teckenrutan.

Kontroll:
  - PDF mot PDF: punktmängderna jämförs cell för cell.
  - PDF mot scan: präglade punkter syns som en ljus/mörk skuggning. Vi vet ur PDF:en exakt
    var punkter ska och inte ska finnas (alla 6 lägen i varje cell) och mäter reliefsignalen
    i varje läge. Tröskeln sätts adaptivt per rad: närvarande lägen mot tomma lägen.

Översättning: svensk punktskrift, grundnivå (bokstäver a–ö, siffror med siffertecken,
versaltecken, komma, punkt, bindestreck). Fullständig översättning (förkortningar,
andra språk) kräver t.ex. liblouis.
"""
from __future__ import annotations

from dataclasses import dataclass, field

import cv2
import numpy as np

# ---------------------------------------------------------------- översättning
_LETTERS = {
    "a": (1,), "b": (1, 2), "c": (1, 4), "d": (1, 4, 5), "e": (1, 5), "f": (1, 2, 4), "g": (1, 2, 4, 5),
    "h": (1, 2, 5), "i": (2, 4), "j": (2, 4, 5), "k": (1, 3), "l": (1, 2, 3), "m": (1, 3, 4),
    "n": (1, 3, 4, 5), "o": (1, 3, 5), "p": (1, 2, 3, 4), "q": (1, 2, 3, 4, 5), "r": (1, 2, 3, 5),
    "s": (2, 3, 4), "t": (2, 3, 4, 5), "u": (1, 3, 6), "v": (1, 2, 3, 6), "w": (2, 4, 5, 6),
    "x": (1, 3, 4, 6), "y": (1, 3, 4, 5, 6), "z": (1, 3, 5, 6),
    "å": (1, 6), "ä": (3, 4, 5), "ö": (2, 4, 6),
}
_PUNCT = {",": (2,), ".": (3,), "-": (3, 6)}
NUMBER_SIGN = frozenset((3, 4, 5, 6))
CAPITAL_SIGN = frozenset((6,))
_DIGITS = "1234567890"   # 1=a ... 0=j

LETTER_OF = {frozenset(v): k for k, v in _LETTERS.items()}
PUNCT_OF = {frozenset(v): k for k, v in _PUNCT.items()}


def text_to_cells(text: str) -> list[frozenset]:
    """Text -> lista av celler (punktmängder). Mellanslag = tom cell."""
    cells: list[frozenset] = []
    in_number = False
    for ch in text:
        if ch == " ":
            cells.append(frozenset())
            in_number = False
        elif ch.isdigit():
            if not in_number:
                cells.append(NUMBER_SIGN)
                in_number = True
            cells.append(frozenset(_LETTERS["abcdefghij"[_DIGITS.index(ch)]]))
        elif ch.lower() in _LETTERS:
            if in_number and ch.lower() in "abcdefghij":
                cells.append(frozenset())       # förenklat: avsluta sifferläge med mellanrum
            in_number = False
            if ch.isupper():
                cells.append(CAPITAL_SIGN)
            cells.append(frozenset(_LETTERS[ch.lower()]))
        elif ch in _PUNCT:
            in_number = False
            cells.append(frozenset(_PUNCT[ch]))
        else:
            raise ValueError(f"tecknet {ch!r} saknas i punktskriftstabellen")
    return cells


def cells_to_text(cells: list[frozenset]) -> str:
    out, in_number, cap = [], False, False
    for c in cells:
        if not c:
            out.append(" ")
            in_number = False
            continue
        if c == NUMBER_SIGN:
            in_number = True
            continue
        if c == CAPITAL_SIGN:
            cap = True
            continue
        if c in LETTER_OF:
            ch = LETTER_OF[c]
            if in_number and ch in "abcdefghij":
                out.append(_DIGITS["abcdefghij".index(ch)])
                continue
            out.append(ch.upper() if cap else ch)
            cap = False
            continue
        if c in PUNCT_OF:
            out.append(PUNCT_OF[c])
            continue
        out.append("?")
    return "".join(out)


def unicode_of(dots) -> str:
    return chr(0x2800 + sum(1 << (n - 1) for n in dots))


def dots_of_unicode(ch: str) -> frozenset:
    v = ord(ch) - 0x2800
    return frozenset(n for n in range(1, 9) if v & (1 << (n - 1)))


# ---------------------------------------------------------------- geometri
@dataclass
class BrailleLine:
    index: int
    top: float                  # y för punktrad 1 (ref px)
    origin: float               # x för vänster punktkolumn i cell 0 (ref px)
    pitch: float                # punktavstånd i cellen (px)
    cell_pitch: float           # cellavstånd (px)
    dot_r: float                # punktradie (px)
    cells: dict = field(default_factory=dict)   # cellindex -> frozenset(punkter)
    kmin: int = 0
    kmax: int = 0
    source: str = "spot"

    def pos(self, k: int, n: int) -> tuple[float, float]:
        col = 0 if n <= 3 else 1
        row = (n - 1) % 3
        return (self.origin + k * self.cell_pitch + col * self.pitch, self.top + row * self.pitch)

    def cell_box(self, k: int) -> tuple[float, float, float, float]:
        r = self.dot_r * 1.2
        return (self.origin + k * self.cell_pitch - r, self.top - r,
                self.origin + k * self.cell_pitch + self.pitch + r, self.top + 2 * self.pitch + r)

    def bbox(self) -> tuple[float, float, float, float]:
        a, b = self.cell_box(self.kmin), self.cell_box(self.kmax)
        return (a[0], a[1], b[2], b[3])

    def cell_list(self) -> list[frozenset]:
        return [self.cells.get(k, frozenset()) for k in range(self.kmin, self.kmax + 1)]

    @property
    def text(self) -> str:
        return cells_to_text(self.cell_list())


def dots_from_spots(spots, geom) -> tuple[list[tuple[float, float]], float]:
    """Punktcentra (ref px) och median-radie ur dekorfärgsbanor med rollen 'braille'."""
    centers, radii = [], []
    for poly in spots.polygons("braille"):
        if len(poly) < 4:
            continue
        xs, ys = geom.pdf_to_px([p[0] for p in poly], [p[1] for p in poly])
        xs, ys = np.asarray(xs, float), np.asarray(ys, float)
        cx, cy = float(xs.mean()), float(ys.mean())
        r = float(np.mean(np.hypot(xs - cx, ys - cy)))
        if r <= 0:
            continue
        centers.append((cx, cy))
        radii.append(r)
    return centers, (float(np.median(radii)) if radii else 0.0)


def dots_from_text(pdf_bytes: bytes, page_index: int, geom) -> tuple[list[tuple[float, float]], float]:
    """Punkter ur Unicode-punktskrift i textlagret (läget i rutan är en approximation)."""
    import pypdfium2 as pdfium
    doc = pdfium.PdfDocument(pdf_bytes)
    centers, sizes = [], []
    try:
        tp = doc[page_index].get_textpage()
        for i in range(tp.count_chars()):
            ch = tp.get_text_range(i, 1)
            if not ch or not (0x2800 <= ord(ch) <= 0x28FF):
                continue
            l, b, r, t = tp.get_charbox(i, loose=True)
            x0, y0 = geom.pdf_to_px(l, t)
            x1, y1 = geom.pdf_to_px(r, b)
            w, h = float(x1 - x0), float(y1 - y0)
            for n in dots_of_unicode(ch):
                if n > 6:
                    continue
                col = 0 if n <= 3 else 1
                row = (n - 1) % 3
                centers.append((float(x0) + w * (0.3 + 0.4 * col), float(y0) + h * (0.25 + 0.25 * row)))
            sizes.append(w)
        tp.close()
    finally:
        doc.close()
    return centers, (0.08 * float(np.median(sizes)) if sizes else 0.0)


def _cluster_1d(vals: np.ndarray, tol: float) -> list[float]:
    vals = np.sort(np.asarray(vals, float))
    if len(vals) == 0:
        return []
    groups, cur = [], [vals[0]]
    for v in vals[1:]:
        if v - cur[-1] <= tol:
            cur.append(v)
        else:
            groups.append(float(np.mean(cur)))
            cur = [v]
    groups.append(float(np.mean(cur)))
    return groups


def decode(dots: list[tuple[float, float]], dot_r: float, ppm: float, source: str = "spot") -> list[BrailleLine]:
    """Punkter -> rader av celler. Varje bokstav har en punkt i rad 1 (punkt 1 eller 4),
    så radens översta punktrad är alltid närvarande."""
    if len(dots) < 2:
        return []
    pts = np.asarray(dots, float)
    d = np.hypot(pts[:, None, 0] - pts[None, :, 0], pts[:, None, 1] - pts[None, :, 1])
    np.fill_diagonal(d, np.inf)
    nn = d.min(axis=1)
    p = float(np.median(nn[nn < 4.0 * ppm])) if np.any(nn < 4.0 * ppm) else 2.5 * ppm
    rows = _cluster_1d(pts[:, 1], p / 3)
    used = [False] * len(rows)
    lines: list[BrailleLine] = []
    for i, top in enumerate(rows):
        if used[i]:
            continue
        members = [j for j, y in enumerate(rows) if not used[j] and top - p / 3 <= y <= top + 2 * p + p / 3]
        for j in members:
            used[j] = True
        lpts = pts[(pts[:, 1] >= top - p / 3) & (pts[:, 1] <= top + 2 * p + p / 3)]
        cols = np.array(_cluster_1d(lpts[:, 0], p / 3))
        # sök cellavstånd c och origo som bäst förklarar kolumnerna
        best = None
        cands = np.arange(2.1, 2.81, 0.01) * p if len(cols) >= 5 else np.array([2.4 * p])
        for c in cands:
            for o0 in np.concatenate([cols, cols - p]):
                rel = cols - o0
                k = np.round(rel / c)
                off = rel - k * c
                res = np.minimum(np.abs(off), np.abs(off - p))
                cost = float(np.sum(res ** 2)) + (0.0 if k.min() >= 0 else 1e9)
                if best is None or cost < best[0] - 1e-6:
                    best = (cost, float(c), float(o0))
        _, c, o = best
        line = BrailleLine(index=len(lines), top=float(top), origin=o, pitch=p, cell_pitch=c, dot_r=dot_r,
                           source=source)
        for x, y in lpts:
            k = int(round((x - o) / c))
            col = 0 if abs((x - o) - k * c) < abs((x - o) - k * c - p) else 1
            row = int(round((y - top) / p))
            row = min(2, max(0, row))
            n = row + 1 + 3 * col
            line.cells[k] = line.cells.get(k, frozenset()) | {n}
        ks = sorted(line.cells)
        line.kmin, line.kmax = ks[0], ks[-1]
        lines.append(line)
    return lines


def from_pdf(spots, pdf_bytes: bytes, page_index: int, geom) -> list[BrailleLine]:
    ppm = geom.px_per_mm()
    dots, r = dots_from_spots(spots, geom)
    src = "spot"
    if not dots:
        dots, r = dots_from_text(pdf_bytes, page_index, geom)
        src = "text"
    if not dots:
        return []
    return decode(dots, r if r > 0 else 0.75 * ppm, ppm, src)


def mask(shape, lines: list[BrailleLine], ppm: float, margin_mm: float = 0.45) -> np.ndarray:
    """Alla sex lägen i varje cell (även tomma), så att prägling inte ger tryckfel."""
    m = np.zeros(shape[:2], np.uint8)
    for ln in lines:
        rad = int(round(ln.dot_r + margin_mm * ppm))
        for k in range(ln.kmin, ln.kmax + 1):
            for n in range(1, 7):
                x, y = ln.pos(k, n)
                cv2.circle(m, (int(round(x)), int(round(y))), rad, 1, -1)
    return m.astype(bool)


# ---------------------------------------------------------------- kontroll i scan
def _diff_for_cell(ln: BrailleLine, k: int, read: frozenset, missing, extra, ppm: float, instance: int,
                   cause: str | None = None) -> dict:
    exp = ln.cells.get(k, frozenset())
    x0, y0, x1, y1 = ln.cell_box(k)
    d = {"instance": instance, "kind": "Braille", "severity": "Critical", "category": "Braille",
         "bbox_px": [int(x0), int(y0), int(x1 - x0), int(y1 - y0)],
         "bbox_mm": [x0 / ppm, y0 / ppm, (x1 - x0) / ppm, (y1 - y0) / ppm],
         "area_mm2": (x1 - x0) * (y1 - y0) / ppm ** 2, "objects": [],
         "metrics": {"delta_l": None, "delta_e": None, "shift_mm": None}, "confidence": 0.85,
         "braille": {"line": ln.index, "cell": k - ln.kmin,
                     "expected": cells_to_text([exp]) if exp else " ",
                     "read": cells_to_text([read]) if read else " ",
                     "expected_unicode": unicode_of(exp), "read_unicode": unicode_of(read),
                     "missing_dots": sorted(missing), "extra_dots": sorted(extra)}}
    if cause:
        d["cause"] = cause
    return d


def _runs(ks: list[int]) -> list[list[int]]:
    out: list[list[int]] = []
    for k in ks:
        if out and k == out[-1][-1] + 1:
            out[-1].append(k)
        else:
            out.append([k])
    return out


def check_scan(resid_L: np.ndarray, lines: list[BrailleLine], ppm: float, instance: int,
               fold_band: np.ndarray | None = None, min_ratio: float = 1.8, min_level: float = 0.8):
    """Kontrollera präglade punkter. resid_L = prov L* − referens L* i referensramen.

    Returnerar (skillnader, läst text per rad, mätvärden).
    """
    diffs, reads, stats = [], [], []
    h, w = resid_L.shape
    wide = None
    if fold_band is not None and fold_band.any():
        k = int(round(1.5 * ppm))
        wide = cv2.dilate(fold_band.astype(np.uint8), np.ones((2 * k + 1, 2 * k + 1), np.uint8)).astype(bool)
    for ln in lines:
        r = ln.dot_r
        x0, y0, x1, y1 = ln.bbox()
        pad = int(3 * ln.pitch)
        X0, Y0 = max(0, int(x0) - pad), max(0, int(y0) - pad)
        X1, Y1 = min(w, int(x1) + pad), min(h, int(y1) + pad)
        sub = resid_L[Y0:Y1, X0:X1].astype(np.float32)
        hp = sub - cv2.GaussianBlur(sub, (0, 0), 2.5 * r)
        energy = cv2.GaussianBlur(np.abs(hp), (0, 0), 0.6 * r)

        def score(x, y):
            xi, yi = int(round(x)) - X0, int(round(y)) - Y0
            if 0 <= xi < energy.shape[1] and 0 <= yi < energy.shape[0]:
                return float(energy[yi, xi])
            return np.nan

        def peak_ratio(x, y):
            """Energi i läget mot en ring runt det: en präglad punkt är en lokal topp,
            en kant från annat innehåll i närheten är det inte."""
            ring = [score(x + 1.5 * r * np.cos(a), y + 1.5 * r * np.sin(a)) for a in np.linspace(0, 2 * np.pi, 8, endpoint=False)]
            ring = [v for v in ring if not np.isnan(v)]
            c = score(x, y)
            return c / max(float(np.median(ring)), 1e-6) if ring else 0.0

        pres, absn, positions = [], [], []
        unsure: set[int] = set()
        for k in range(ln.kmin, ln.kmax + 1):
            exp = ln.cells.get(k, frozenset())
            for n in range(1, 7):
                x, y = ln.pos(k, n)
                if wide is not None and 0 <= int(y) < h and 0 <= int(x) < w and wide[int(y), int(x)]:
                    unsure.add(k)        # vecket skuggar punkten: kan inte bedömas
                    continue
                s = score(x, y)
                if np.isnan(s):
                    continue
                positions.append((k, n, s, n in exp))
                (pres if n in exp else absn).append(s)
        # osäkra celler (veck) rapporteras som sammanhängande zoner
        for run in _runs(sorted(unsure)):
            a, b = ln.cell_box(run[0]), ln.cell_box(run[-1])
            diffs.append({"instance": instance, "kind": "Uncertain", "severity": "Info", "category": "Braille",
                          "cause": "fold", "bbox_px": [int(a[0]), int(a[1]), int(b[2] - a[0]), int(b[3] - a[1])],
                          "bbox_mm": [a[0] / ppm, a[1] / ppm, (b[2] - a[0]) / ppm, (b[3] - a[1]) / ppm],
                          "area_mm2": (b[2] - a[0]) * (b[3] - a[1]) / ppm ** 2, "objects": [],
                          "metrics": {"delta_l": None, "delta_e": None, "shift_mm": None}, "confidence": 0.5,
                          "braille": {"line": ln.index, "cells": [k - ln.kmin for k in run]}})
        if not pres:
            reads.append(None)
            continue
        P = float(np.median(pres))
        A = float(np.median(absn)) if absn else 0.25 * P
        stats.append({"line": ln.index, "present": round(P, 2), "absent": round(A, 2)})
        if P < min_ratio * A or P < min_level:
            # ingen prägling alls (eller ej mätbar)
            bx0, by0, bx1, by1 = ln.bbox()
            diffs.append({"instance": instance, "kind": "Braille", "severity": "Critical", "category": "Braille",
                          "cause": "embossing_missing",
                          "bbox_px": [int(bx0), int(by0), int(bx1 - bx0), int(by1 - by0)],
                          "bbox_mm": [bx0 / ppm, by0 / ppm, (bx1 - bx0) / ppm, (by1 - by0) / ppm],
                          "area_mm2": (bx1 - bx0) * (by1 - by0) / ppm ** 2, "objects": [],
                          "metrics": {"delta_l": None, "delta_e": None, "shift_mm": None}, "confidence": 0.7,
                          "braille": {"line": ln.index, "expected_text": ln.text, "read_text": None}})
            reads.append(None)
            continue
        t_miss = A + 0.40 * (P - A)
        t_extra = A + 0.60 * (P - A)
        observed: dict[int, set] = {}
        missing: dict[int, set] = {}
        extra: dict[int, set] = {}
        for k, n, s, exp in positions:
            is_extra = (not exp) and s > t_extra and peak_ratio(*ln.pos(k, n)) > 1.4
            if exp and s >= t_miss or is_extra:
                observed.setdefault(k, set()).add(n)
            if exp and s < t_miss:
                missing.setdefault(k, set()).add(n)
            if is_extra:
                extra.setdefault(k, set()).add(n)
        read_cells = [frozenset(observed.get(k, set())) if k not in unsure else None for k in range(ln.kmin, ln.kmax + 1)]
        reads.append("".join("~" if c is None else cells_to_text([c]) for c in read_cells))
        for k in sorted((set(missing) | set(extra)) - unsure):
            dcell = _diff_for_cell(ln, k, frozenset(observed.get(k, set())), missing.get(k, ()),
                                   extra.get(k, ()), ppm, instance)
            # signal: präglingens kontrast (präglat/tomt) och hur tydligt avvikande lägen ligger
            # på fel sida om tröskeln (0 = på tröskeln, 1 = som en typisk präglad/tom punkt)
            marg = [((t_miss - s) / max(P - A, 1e-6)) / 0.4 for kk, n, s, e in positions if kk == k and e and s < t_miss]
            marg += [((s - t_extra) / max(P - A, 1e-6)) / 0.4 for kk, n, s, e in positions
                     if kk == k and not e and n in extra.get(k, ())]
            dcell["braille"]["contrast"] = round(P / max(A, 1e-6), 2)
            dcell["braille"]["margin"] = round(float(min(marg)) if marg else 0.0, 2)
            diffs.append(dcell)
    return diffs, reads, stats


def compare_dots(lines: list[BrailleLine], sample_dots_ref: list[tuple[float, float]], ppm: float,
                 instance: int = 1) -> tuple[list[dict], list[str]]:
    """PDF mot PDF: provets punkter (redan i referensramen) mot referensens celler."""
    diffs, reads = [], []
    sd = np.asarray(sample_dots_ref, float).reshape(-1, 2)
    used = np.zeros(len(sd), bool)
    for ln in lines:
        tol = 0.35 * ln.pitch
        observed: dict[int, set] = {}
        for k in range(ln.kmin - 1, ln.kmax + 2):
            for n in range(1, 7):
                x, y = ln.pos(k, n)
                if len(sd):
                    dd = np.hypot(sd[:, 0] - x, sd[:, 1] - y)
                    j = int(np.argmin(dd))
                    if dd[j] <= tol:
                        observed.setdefault(k, set()).add(n)
                        used[j] = True
        ks = range(min([ln.kmin] + list(observed)), max([ln.kmax] + list(observed)) + 1)
        reads.append(cells_to_text([frozenset(observed.get(k, set())) for k in ks]))
        for k in ks:
            exp = ln.cells.get(k, frozenset())
            got = frozenset(observed.get(k, set()))
            if got != exp:
                diffs.append(_diff_for_cell(ln, k, got, exp - got, got - exp, ppm, instance))
    for j in np.nonzero(~used)[0]:
        x, y = sd[j]
        r = lines[0].dot_r if lines else 0.75 * ppm
        diffs.append({"instance": instance, "kind": "Braille", "severity": "Critical", "category": "Braille",
                      "cause": "extra_dot_outside_cells",
                      "bbox_px": [int(x - 2 * r), int(y - 2 * r), int(4 * r), int(4 * r)],
                      "bbox_mm": [(x - 2 * r) / ppm, (y - 2 * r) / ppm, 4 * r / ppm, 4 * r / ppm],
                      "area_mm2": 16 * r * r / ppm ** 2, "objects": [],
                      "metrics": {"delta_l": None, "delta_e": None, "shift_mm": None}, "confidence": 0.9})
    return diffs, reads
