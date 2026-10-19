"""Objektkarta från PDF:en (text, bild, vektor) – motsvarar pdf/objmap i kärnan.

Text tas från PDF:ens textlager (Unicode), aldrig via OCR.
Rutor anges i referensens pixelkoordinater (övre vänster origo).
"""
from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np
import pypdfium2 as pdfium
import pypdfium2.raw as pdfium_c

from .render import PageGeometry


@dataclass
class ObjEntry:
    id: int
    kind: str                    # Text | Image | Vector | Barcode
    bbox: tuple                  # (x0, y0, x1, y1) px
    text: str = ""
    extra: dict = field(default_factory=dict)


def _bbox_px(geom: PageGeometry, l, b, r, t):
    x0, y0 = geom.pdf_to_px(l, t)
    x1, y1 = geom.pdf_to_px(r, b)
    return (float(x0), float(y0), float(x1), float(y1))


def build(pdf_bytes: bytes, page_index: int, geom: PageGeometry, exclude_boxes=()) -> list[ObjEntry]:
    """Objektkarta. pdf_bytes bör vara den sanerade PDF:en (utan stanslinje, lack, punktskrift).

    exclude_boxes: rutor (ref px) där vektorer inte ska tolkas som glyfer, t.ex. streckkoder.
    """
    doc = pdfium.PdfDocument(pdf_bytes)
    objs: list[ObjEntry] = []
    try:
        page = doc[page_index]
        # --- text: tecken -> ord ---
        tp = page.get_textpage()
        n = tp.count_chars()
        word, wbox = "", None
        last_r, last_base = None, None

        def flush():
            nonlocal word, wbox
            if word.strip() and wbox is not None:
                objs.append(ObjEntry(len(objs) + 1, "Text", _bbox_px(geom, *wbox), word.strip()))
            word, wbox = "", None

        for i in range(n):
            ch = tp.get_text_range(i, 1)
            l, b, r, t = tp.get_charbox(i, loose=False)
            if ch.isspace() or ch in ("\r", "\n") or (r - l) <= 0:
                flush()
                last_r = None
                continue
            # skiljetecken (komma, punkt) har små rutor: jämför mot ordets höjd hittills
            size = max(t - b, (wbox[3] - wbox[1]) if wbox is not None else 0.0, 1.0)
            if last_r is not None and (l - last_r > 0.6 * size or abs(b - last_base) > 0.6 * size):
                flush()
            word += ch
            wbox = (l, b, r, t) if wbox is None else (min(wbox[0], l), min(wbox[1], b), max(wbox[2], r), max(wbox[3], t))
            last_r, last_base = r, b
        flush()
        tp.close()

        # --- övriga sidobjekt ---
        for o in page.get_objects(max_depth=4):
            if o.type == pdfium_c.FPDF_PAGEOBJ_IMAGE:
                kind = "Image"
            elif o.type in (pdfium_c.FPDF_PAGEOBJ_PATH, pdfium_c.FPDF_PAGEOBJ_SHADING):
                kind = "Vector"
            else:
                continue
            l, b, r, t = o.get_bounds() if hasattr(o, "get_bounds") else o.get_pos()
            box = _bbox_px(geom, l, b, r, t)
            w_, h_ = box[2] - box[0], box[3] - box[1]
            if max(w_, h_) < 2:
                continue
            ppm = geom.px_per_mm()
            if kind == "Vector" and _inside_any(box, exclude_boxes, pad=0.5 * ppm):
                kind = "Barcode"         # staplar/moduler i en streckkod
            if kind == "Vector" and min(w_, h_) <= 0.5 * ppm and max(w_, h_) >= max(3 * ppm, 8 * min(w_, h_)):
                kind = "Rule"            # tunn linje: tabellinje, avdelare, ram
                t_min = 0.3 * ppm        # streckade linjer kan ha nolltjocka rutor
                if w_ < t_min:
                    box = (box[0] - t_min / 2, box[1], box[2] + t_min / 2, box[3])
                if h_ < t_min:
                    box = (box[0], box[1] - t_min / 2, box[2], box[3] + t_min / 2)
            objs.append(ObjEntry(len(objs) + 1, kind, box))
    finally:
        doc.close()
    objs = _group_outlined_text(objs, geom.px_per_mm(), exclude_boxes)
    for i, o in enumerate(objs):
        o.id = i + 1
    return objs


def _inside_any(box, boxes, pad=0.0):
    cx, cy = (box[0] + box[2]) / 2, (box[1] + box[3]) / 2
    return any(b[0] - pad <= cx <= b[2] + pad and b[1] - pad <= cy <= b[3] + pad for b in boxes)


def _group_outlined_text(objs: list[ObjEntry], ppm: float, exclude_boxes) -> list[ObjEntry]:
    """Konturerad text: rader av glyfstora vektorer med gemensam baslinje.

    Kriterier: höjd 0,8–7 mm, bredd ≤ 1,6 × höjd, minst 3 i rad, glapp ≤ 0,6 × höjd,
    vertikal överlapp ≥ 50 %, och bottnarna (baslinjen) samlade – slumpvisa former
    (ikoner, illustrationer) har spridda bottnar.
    """
    cand = []
    for i, o in enumerate(objs):
        if o.kind != "Vector":
            continue
        w_, h_ = o.bbox[2] - o.bbox[0], o.bbox[3] - o.bbox[1]
        if not (0.8 * ppm <= h_ <= 7.0 * ppm) or w_ > 1.6 * h_ or w_ < 0.12 * ppm:
            continue
        if _inside_any(o.bbox, exclude_boxes, pad=1.0 * ppm):
            continue
        cand.append(i)
    if len(cand) < 3:
        return objs
    parent = {i: i for i in cand}

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    for ii, a in enumerate(cand):
        A = objs[a].bbox
        for b in cand[ii + 1:]:
            B = objs[b].bbox
            ha, hb = A[3] - A[1], B[3] - B[1]
            hm = max(ha, hb)
            if max(ha, hb) > 3.0 * min(ha, hb):
                continue
            ov = min(A[3], B[3]) - max(A[1], B[1])
            if ov < 0.5 * min(ha, hb):
                continue
            gap = max(B[0] - A[2], A[0] - B[2])
            if gap > 0.6 * hm:
                continue
            parent[find(a)] = find(b)
    groups: dict[int, list[int]] = {}
    for i in cand:
        groups.setdefault(find(i), []).append(i)
    remove, new = set(), []
    for members in groups.values():
        if len(members) < 3:
            continue
        boxes = np.array([objs[i].bbox for i in members])
        hs = boxes[:, 3] - boxes[:, 1]
        bottoms = boxes[:, 3]
        med = np.median(bottoms)
        # baslinje: de flesta glyfer slutar på samma höjd (nedstaplar tillåts)
        on_base = np.abs(bottoms - med) <= 0.12 * np.median(hs)
        if on_base.mean() < 0.6:
            continue
        x0, y0 = boxes[:, 0].min(), boxes[:, 1].min()
        x1, y1 = boxes[:, 2].max(), boxes[:, 3].max()
        glyphs = sorted([tuple(float(v) for v in objs[i].bbox) for i in members])
        new.append(ObjEntry(0, "OutlinedText", (float(x0), float(y0), float(x1), float(y1)),
                            extra={"glyphs": glyphs, "baseline_px": float(med)}))
        remove.update(members)
    return [o for i, o in enumerate(objs) if i not in remove] + new


def overlapping(objs: list[ObjEntry], box, kinds=None, min_frac=0.05):
    """Objekt vars ruta överlappar box (minst min_frac av box-ytan)."""
    x0, y0, x1, y1 = box
    area = max((x1 - x0) * (y1 - y0), 1.0)
    out = []
    for o in objs:
        if kinds and o.kind not in kinds:
            continue
        ix = max(0.0, min(x1, o.bbox[2]) - max(x0, o.bbox[0]))
        iy = max(0.0, min(y1, o.bbox[3]) - max(y0, o.bbox[1]))
        if ix * iy >= min_frac * area:
            out.append(o)
    return out
