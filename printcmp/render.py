"""Rendering av PDF med PDFium (pypdfium2) – motsvarar pdf/render i kärnan.

Gränssnittet är avsiktligt litet så att en MuPDF-renderare kan kopplas in
utan att resten av pipelinen ändras.
"""
from __future__ import annotations

from dataclasses import dataclass

import numpy as np
import pypdfium2 as pdfium


@dataclass
class PageGeometry:
    box: tuple          # (left, bottom, right, top) i PDF-punkter, den använda boxen
    dpi: float

    @property
    def width_mm(self) -> float:
        return (self.box[2] - self.box[0]) * 25.4 / 72

    @property
    def height_mm(self) -> float:
        return (self.box[3] - self.box[1]) * 25.4 / 72

    def pdf_to_px(self, x, y):
        s = self.dpi / 72.0
        return ((np.asarray(x) - self.box[0]) * s, (self.box[3] - np.asarray(y)) * s)

    def px_per_mm(self) -> float:
        return self.dpi / 25.4


@dataclass
class Rendered:
    rgb: np.ndarray     # HxWx3 uint8
    geom: PageGeometry


class PdfiumRenderer:
    name = "pdfium"

    def render(self, pdf_bytes: bytes, page_index: int, dpi: float, box: str = "trim") -> Rendered:
        doc = pdfium.PdfDocument(pdf_bytes)
        try:
            page = doc[page_index]
            getters = {"trim": page.get_trimbox, "media": page.get_mediabox,
                       "crop": page.get_cropbox, "art": page.get_artbox}
            b = getters[box]()
            cb = page.get_cropbox()
            crop = (b[0] - cb[0], b[1] - cb[1], cb[2] - b[2], cb[3] - b[3])
            crop = tuple(max(0.0, c) for c in crop)
            bitmap = page.render(scale=dpi / 72.0, crop=crop, rev_byteorder=True,
                                 fill_color=(255, 255, 255, 255))
            arr = bitmap.to_numpy()
            if arr.shape[2] == 4:
                arr = arr[:, :, :3]
            return Rendered(np.ascontiguousarray(arr), PageGeometry(tuple(b), dpi))
        finally:
            doc.close()


def get_renderer(name: str = "pdfium"):
    if name == "pdfium":
        return PdfiumRenderer()
    raise ValueError(f"okänd renderare: {name} (MuPDF kan läggas till här)")
