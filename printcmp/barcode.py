"""Streckkoder (avsnitt 6.10). Prototypen använder OpenCV:s detektorer
(EAN/UPC/Code128 via cv2.barcode, QR via QRCodeDetector). DataMatrix stöds inte här
– zxing-cpp kopplas in när det finns tillgängligt.
"""
from __future__ import annotations

from dataclasses import dataclass

import cv2
import numpy as np


@dataclass
class Code:
    symbology: str
    data: str
    bbox: tuple       # x0, y0, x1, y1 i bildens px


def _bbox(points) -> tuple:
    p = np.asarray(points).reshape(-1, 2)
    return (float(p[:, 0].min()), float(p[:, 1].min()), float(p[:, 0].max()), float(p[:, 1].max()))


def detect(rgb: np.ndarray, dpi: float, work_dpi: float = 300.0) -> list[Code]:
    s = min(1.0, work_dpi / dpi)
    gray = cv2.cvtColor(rgb, cv2.COLOR_RGB2GRAY)
    if s < 1.0:
        gray = cv2.resize(gray, None, fx=s, fy=s, interpolation=cv2.INTER_AREA)
    codes: list[Code] = []
    try:
        det = cv2.barcode.BarcodeDetector()
        ok, infos, types, pts = det.detectAndDecodeWithType(gray)
        if ok and pts is not None:
            for info, typ, p in zip(infos, types, pts):
                if info:
                    b = _bbox(p)
                    codes.append(Code(str(typ), str(info), tuple(v / s for v in b)))
    except cv2.error:
        pass
    try:
        qr = cv2.QRCodeDetector()
        ok, infos, pts, _ = qr.detectAndDecodeMulti(gray)
        if ok and pts is not None:
            for info, p in zip(infos, pts):
                if info:
                    b = _bbox(p)
                    codes.append(Code("QR", str(info), tuple(v / s for v in b)))
    except cv2.error:
        pass
    return codes


def decode_region(rgb: np.ndarray, box, pad: int, dpi: float) -> list[Code]:
    """Avkoda koder kring box. Provar växande marginal (tyst zon) och till sist hela bilden."""
    h, w = rgb.shape[:2]
    ppm = dpi / 25.4
    bx0, by0, bx1, by1 = (int(round(v)) for v in box)
    for p in (pad, int(5 * ppm), int(10 * ppm)):
        x0, y0 = max(0, bx0 - p), max(0, by0 - p)
        x1, y1 = min(w, bx1 + p), min(h, by1 + p)
        crop = rgb[y0:y1, x0:x1]
        if crop.size == 0:
            continue
        out = detect(crop, dpi)
        for c in out:
            c.bbox = (c.bbox[0] + x0, c.bbox[1] + y0, c.bbox[2] + x0, c.bbox[3] + y0)
        if out:
            return out
    # sista försök: hela bilden, behåll koder som överlappar rutan
    out = detect(rgb, dpi)
    return [c for c in out if not (c.bbox[2] < bx0 or bx1 < c.bbox[0] or c.bbox[3] < by0 or by1 < c.bbox[1])]
