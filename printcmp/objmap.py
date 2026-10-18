"""Objektkarta från PDF:en (text, bild, vektor) – motsvarar pdf/objmap i kärnan.

Text tas från PDF:ens textlager (Unicode), aldrig via OCR.
Rutor anges i referensens pixelkoordinater (övre vänster origo).
"""
from __future__ import annotations

from dataclasses import dataclass, field

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


def build(pdf_bytes: bytes, page_index: int, geom: PageGeometry) -> list[ObjEntry]:
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
            size = max(t - b, 1.0)
            if last_r is not None and (l - last_r > 0.6 * size or abs(b - last_base) > 0.5 * size):
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
            if (box[2] - box[0]) * (box[3] - box[1]) < 4:
                continue
            objs.append(ObjEntry(len(objs) + 1, kind, box))
    finally:
        doc.close()
    return objs


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
