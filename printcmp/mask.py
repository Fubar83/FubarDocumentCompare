"""Masker (avsnitt 6.3): utanför stanslinje, variabel data, användarmasker, lack/folie."""
from __future__ import annotations

from dataclasses import dataclass

import cv2
import numpy as np

from .pdfspots import SpotAnalysis, polygon_area
from .render import PageGeometry


@dataclass
class Masks:
    valid: np.ndarray        # bool: jämförs
    inside: np.ndarray       # bool: inom stanslinjen (före erodering) – används för lokalisering
    varnish: np.ndarray      # bool: endast form jämförs, inte färg
    variable: np.ndarray     # bool: variabel data (ignoreras)
    has_dieline: bool


def _poly_px(poly, geom: PageGeometry):
    xs, ys = geom.pdf_to_px([p[0] for p in poly], [p[1] for p in poly])
    return np.stack([xs, ys], axis=1).astype(np.float32)


def build(shape, geom: PageGeometry, spots: SpotAnalysis, user_masks: list, erode_mm: float = 0.3) -> Masks:
    h, w = shape[:2]
    ppm = geom.px_per_mm()
    inside = np.zeros((h, w), np.uint8)
    dieline_polys = spots.polygons("dieline")
    # Största slutna polygonen räknas som stanslinje; övriga (hål, perforering) dras av.
    closed = [p for p in dieline_polys if len(p) >= 3]
    closed.sort(key=polygon_area, reverse=True)
    has_dieline = bool(closed)
    if has_dieline:
        outer = _poly_px(closed[0], geom)
        cv2.fillPoly(inside, [np.round(outer * 16).astype(np.int32)], 1, lineType=cv2.LINE_8, shift=4)
        for p in closed[1:]:
            pp = _poly_px(p, geom)
            if polygon_area(p) > 0.02 * polygon_area(closed[0]):
                cv2.fillPoly(inside, [np.round(pp * 16).astype(np.int32)], 0, shift=4)
    else:
        inside[:] = 1
    inside_b = inside.astype(bool)

    er = max(1, int(round(erode_mm * ppm)))
    k = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * er + 1, 2 * er + 1))
    valid = cv2.erode(inside, k).astype(bool)
    if not has_dieline:
        valid[:er, :] = valid[-er:, :] = False
        valid[:, :er] = valid[:, -er:] = False

    variable = np.zeros((h, w), bool)
    for m in user_masks:
        x, y, mw, mh = m["rect_mm"]
        x0, y0 = int(np.floor(x * ppm)), int(np.floor(y * ppm))
        x1, y1 = int(np.ceil((x + mw) * ppm)), int(np.ceil((y + mh) * ppm))
        variable[max(0, y0):min(h, y1), max(0, x0):min(w, x1)] = True
    # Variabel data placeras inte exakt: lägg till marginal (0,5 mm) runt rutan.
    vm = max(1, int(round(0.5 * ppm)))
    variable = cv2.dilate(variable.astype(np.uint8), np.ones((2 * vm + 1, 2 * vm + 1), np.uint8)).astype(bool)
    valid &= ~variable

    varnish = np.zeros((h, w), np.uint8)
    for p in spots.polygons("varnish"):
        if len(p) >= 3:
            cv2.fillPoly(varnish, [np.round(_poly_px(p, geom) * 16).astype(np.int32)], 1, shift=4)
    return Masks(valid=valid, inside=inside_b, varnish=varnish.astype(bool), variable=variable, has_dieline=has_dieline)
