"""Diff (avsnitt 6.7): struktur och färg på registrerade, normaliserade bilder.

Struktur: provets L* jämförs mot referensens min/max i ett grannskap med radie
r_edge (och tvärtom). Det tål små kantförskjutningar men fångar saknade eller
tillagda detaljer, även tunna linjer. SSIM används som tillägg i texturerade ytor.

Färg: CIEDE2000 mellan utjämnade Lab-bilder, bara i ytor utan starka kanter.
"""
from __future__ import annotations

from dataclasses import dataclass

import cv2
import numpy as np
from skimage.color import deltaE_ciede2000, rgb2lab
from skimage.metrics import structural_similarity


@dataclass
class DiffMaps:
    ref_lab: np.ndarray
    smp_lab: np.ndarray
    d_struct: np.ndarray     # L*-avvikelse utanför grannskapsintervall
    ssim: np.ndarray
    delta_e: np.ndarray
    struct_mask: np.ndarray
    color_mask: np.ndarray


@dataclass
class Blob:
    bbox: tuple              # x0, y0, x1, y1 (ref px, exklusiv)
    area_px: int
    mask: np.ndarray         # bool, bbox-utsnitt
    kind_hint: str           # "struct" | "color"
    max_dl: float
    mean_de: float
    mean_signed_dl: float    # medel(prov L - ref L) inom flaggade pixlar


def _range_dist(a: np.ndarray, b: np.ndarray, radius: int) -> np.ndarray:
    """Hur mycket a ligger utanför [min, max] av b i grannskapet."""
    if radius <= 0:
        return np.abs(a - b)
    k = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * radius + 1, 2 * radius + 1))
    bmax = cv2.dilate(b, k)
    bmin = cv2.erode(b, k)
    return np.maximum(a - bmax, 0) + np.maximum(bmin - a, 0)


def compute(ref_f: np.ndarray, smp_f: np.ndarray, valid: np.ndarray, varnish: np.ndarray,
            ppm: float, tol: dict) -> DiffMaps:
    ref_lab = rgb2lab(ref_f).astype(np.float32)
    smp_lab = rgb2lab(smp_f).astype(np.float32)
    rL, sL = ref_lab[..., 0], smp_lab[..., 0]
    radius = int(round(tol["edge_radius_mm"] * ppm))
    d1 = _range_dist(sL, rL, radius)       # prov utanför ref-intervallet (tillagt / för mörkt)
    d2 = _range_dist(rL, sL, radius)       # ref utanför prov-intervallet (saknat i provet)
    d_struct = np.maximum(d1, d2)
    d_struct = cv2.GaussianBlur(d_struct, (0, 0), 0.7)

    ssim = structural_similarity(rL, sL, data_range=100.0, gaussian_weights=True, sigma=1.5,
                                 use_sample_covariance=False, full=True)[1].astype(np.float32)
    absdl = np.abs(cv2.GaussianBlur(sL - rL, (0, 0), 1.0))
    struct = ((d_struct > tol["struct_dl"]) & (ssim < 0.95)) | (d_struct > tol["struct_dl_strong"]) | \
             ((ssim < tol["ssim"]) & (absdl > 0.75 * tol["struct_dl"]) & (d_struct > 0.5 * tol["struct_dl"]))
    struct &= valid

    k = max(3, int(round(tol["color_window_mm"] * ppm)) | 1)
    rb = cv2.blur(ref_lab, (k, k))
    sb = cv2.blur(smp_lab, (k, k))
    de = deltaE_ciede2000(rb, sb).astype(np.float32)
    # Färg bedöms i jämna ytor: maska bort närhet till kanter i båda bilderna.
    gr = cv2.morphologyEx(rL, cv2.MORPH_GRADIENT, np.ones((3, 3), np.uint8))
    gs = cv2.morphologyEx(sL, cv2.MORPH_GRADIENT, np.ones((3, 3), np.uint8))
    edgy = cv2.dilate(((gr > 8) | (gs > 8)).astype(np.uint8), np.ones((k, k), np.uint8)) > 0
    # Mycket mörka ytor (svart) har instabil kulörton i scannern – bedöms bara på ljushet.
    dark = (rb[..., 0] < 25) & (sb[..., 0] < 25)
    color = (de > tol["delta_e"]) & valid & ~edgy & ~varnish & ~dark
    return DiffMaps(ref_lab, smp_lab, d_struct, ssim, de, struct, color)


def blobs(maps: DiffMaps, ppm: float, tol: dict) -> list[Blob]:
    out: list[Blob] = []
    merge = max(1, int(round(tol["merge_mm"] * ppm)))
    for kind, m, open_k in (("struct", maps.struct_mask, 2), ("color", maps.color_mask, 3)):
        min_area = (tol["min_blob_mm2"] if kind == "struct" else tol["min_color_blob_mm2"]) * ppm * ppm
        mm = cv2.morphologyEx(m.astype(np.uint8), cv2.MORPH_OPEN, np.ones((open_k, open_k), np.uint8))
        if kind == "color":
            mm &= ~(cv2.dilate(maps.struct_mask.astype(np.uint8), np.ones((5, 5), np.uint8)))
        grouped = cv2.dilate(mm, cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * merge + 1, 2 * merge + 1)))
        n, labels, stats, _ = cv2.connectedComponentsWithStats(grouped, connectivity=8)
        for i in range(1, n):
            x, y, w, h, _ = stats[i]
            sub = (labels[y:y + h, x:x + w] == i) & (mm[y:y + h, x:x + w] > 0)
            area = int(sub.sum())
            if area < min_area:
                continue
            dl = maps.d_struct[y:y + h, x:x + w][sub]
            de = maps.delta_e[y:y + h, x:x + w][sub]
            sdl = (maps.smp_lab[y:y + h, x:x + w, 0] - maps.ref_lab[y:y + h, x:x + w, 0])[sub]
            out.append(Blob((x, y, x + w, y + h), area, sub, kind, float(dl.max()), float(de.mean()), float(sdl.mean())))
    return out
