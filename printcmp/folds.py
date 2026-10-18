"""Veckdetektion (avsnitt 6.6).

Ett veck syns som ett långt, rakt band där provet avviker från referensen
(skugga på ena sidan, ljus på andra). Vi letar raka linjer i residualen
och kräver att residualen syns även där referensen är ljus papper – så att
en saknad tryckt linje inte tolkas som veck.
"""
from __future__ import annotations

from dataclasses import dataclass

import cv2
import numpy as np
from skimage.transform import probabilistic_hough_line


@dataclass
class Fold:
    p0: tuple   # (x, y) ref px
    p1: tuple
    band_px: float


def detect(ref_L: np.ndarray, smp_L: np.ndarray, valid: np.ndarray, ppm: float, band_mm: float) -> tuple[list[Fold], np.ndarray]:
    h, w = ref_L.shape
    ds = max(1, int(round(ppm / 6)))          # ~6 px/mm
    r = cv2.resize(ref_L, (w // ds, h // ds), interpolation=cv2.INTER_AREA)
    s = cv2.resize(smp_L, (w // ds, h // ds), interpolation=cv2.INTER_AREA)
    v = cv2.resize(valid.astype(np.uint8), (w // ds, h // ds), interpolation=cv2.INTER_NEAREST).astype(bool)
    res = cv2.GaussianBlur(s - r, (0, 0), 1.0)
    # Ta bort lågfrekvent rest
    res = res - cv2.GaussianBlur(res, (0, 0), 12)
    mag = np.abs(res)
    edges_ref = cv2.Canny(np.clip(r * 2.55, 0, 255).astype(np.uint8), 30, 90) > 0
    edges_ref = cv2.dilate(edges_ref.astype(np.uint8), np.ones((3, 3), np.uint8)) > 0
    # brusanpassade trösklar (raster i tonplattor ger annars "veck" överallt)
    flat = v & ~edges_ref
    sigma = 1.4826 * float(np.median(mag[flat])) if flat.any() else 1.0
    t_cand = max(2.5, 4.0 * sigma)
    t_line = max(2.0, 3.0 * sigma)
    cand = (mag > t_cand) & v & ~edges_ref
    cand = cv2.morphologyEx(cand.astype(np.uint8), cv2.MORPH_OPEN, np.ones((2, 2), np.uint8)).astype(bool)
    min_len = int(0.45 * min(cand.shape))
    if min_len < 20:
        return [], np.zeros((h, w), bool)
    lines = probabilistic_hough_line(cand, threshold=10, line_length=min_len, line_gap=max(4, min_len // 6),
                                     rng=1)
    folds: list[Fold] = []
    band = np.zeros((h, w), np.uint8)
    bw = band_mm * ppm
    H_, W_ = cand.shape
    vy, vx = np.nonzero(v)
    vb = (vx.min(), vy.min(), vx.max(), vy.max()) if len(vx) else (0, 0, W_ - 1, H_ - 1)
    edge_tol = 3.0 * ppm / ds
    for (x0, y0), (x1, y1) in lines:
        n = int(np.hypot(x1 - x0, y1 - y0))
        if n < min_len:
            continue
        # Linjer längs etikettens kant är stansning/kantskugga, inte veck
        ang = np.degrees(np.arctan2(y1 - y0, x1 - x0)) % 180
        if min(ang, 180 - ang) < 5 and min(abs((y0 + y1) / 2 - vb[1]), abs((y0 + y1) / 2 - vb[3])) < edge_tol:
            continue
        if abs(ang - 90) < 5 and min(abs((x0 + x1) / 2 - vb[0]), abs((x0 + x1) / 2 - vb[2])) < edge_tol:
            continue
        # Ett veck korsar hela etiketten: förläng linjen över bilden och kräv att
        # residualen finns längs (nästan) hela den giltiga sträckan, även på ljust papper.
        dx, dy = (x1 - x0) / n, (y1 - y0) / n
        L = int(np.hypot(W_, H_))
        ts = np.arange(-L, L)
        xs = np.round(x0 + ts * dx).astype(int)
        ys = np.round(y0 + ts * dy).astype(int)
        ok = (xs >= 0) & (xs < W_) & (ys >= 0) & (ys < H_)
        xs, ys = xs[ok], ys[ok]
        vv = v[ys, xs]
        if vv.sum() < min_len:
            continue
        xs, ys = xs[vv], ys[vv]
        # tillåt ±1 px avvikelse från linjen
        m_line = np.maximum.reduce([np.maximum(mag[np.clip(ys + o, 0, H_ - 1), xs], mag[ys, np.clip(xs + o, 0, W_ - 1)]) for o in (-2, -1, 0, 1, 2)])
        bright = (r[ys, xs] > 85) & ~edges_ref[ys, xs]
        if bright.mean() < 0.25:
            continue                                  # längs tryckt yta: troligen ett riktigt fel
        if (m_line[bright] > t_line).mean() < 0.6:
            continue
        # Förfina linjen: anpassa (PCA) till residualpixlar nära den förlängda linjen.
        # Hough-segmenten har grov vinkel, och vinkeln avgör var vecket hamnar i andra instanser.
        yy_, xx_ = np.nonzero((mag > t_line) & v & ~edges_ref)
        if len(xx_) > 50:
            pts = np.stack([xx_, yy_], 1).astype(np.float64)
            a0 = np.array([x0, y0], float)
            dvec = np.array([dx, dy])
            nvec = np.array([-dy, dx])
            for _ in range(3):
                dist = np.abs((pts - a0) @ nvec)
                near = pts[dist < 3.0]
                if len(near) < 30:
                    break
                mu = near.mean(axis=0)
                _, _, vt = np.linalg.svd(near - mu, full_matrices=False)
                dvec = vt[0]
                nvec = np.array([-dvec[1], dvec[0]])
                a0 = mu
            proj = (pts[np.abs((pts - a0) @ nvec) < 3.0] - a0) @ dvec
            if len(proj) > 30:
                q0, q1 = a0 + proj.min() * dvec, a0 + proj.max() * dvec
                x0, y0, x1, y1 = q0[0], q0[1], q1[0], q1[1]
        p0 = (x0 * ds, y0 * ds)
        p1 = (x1 * ds, y1 * ds)
        # slå ihop med befintligt veck om nästan parallellt och nära
        merged = False
        for f in folds:
            d = _point_line_dist(np.array(p0), np.array(f.p0), np.array(f.p1))
            if d < 2 * bw and _angle_diff(p0, p1, f.p0, f.p1) < 5:
                merged = True
                break
        if merged:
            continue
        folds.append(Fold(p0, p1, bw))
    for f in folds:
        _draw_extended_band(band, f, w, h)
    return folds, band.astype(bool)


def _point_line_dist(p, a, b):
    d = b - a
    t = np.dot(p - a, d) / max(np.dot(d, d), 1e-9)
    return float(np.linalg.norm(a + t * d - p))


def _angle_diff(a0, a1, b0, b1):
    ta = np.degrees(np.arctan2(a1[1] - a0[1], a1[0] - a0[0])) % 180
    tb = np.degrees(np.arctan2(b1[1] - b0[1], b1[0] - b0[0])) % 180
    d = abs(ta - tb)
    return min(d, 180 - d)


def _draw_extended_band(img, f: Fold, w, h):
    (x0, y0), (x1, y1) = f.p0, f.p1
    dx, dy = x1 - x0, y1 - y0
    n = np.hypot(dx, dy) or 1.0
    ux, uy = dx / n, dy / n
    L = 2 * (w + h)
    a = (int(x0 - ux * L), int(y0 - uy * L))
    b = (int(x1 + ux * L), int(y1 + uy * L))
    cv2.line(img, a, b, 1, thickness=max(1, int(2 * f.band_px)))


def refine_near(ref_L: np.ndarray, smp_L: np.ndarray, valid: np.ndarray, ppm: float, prior: Fold,
                search_mm: float = 4.0) -> Fold:
    """Anpassa en känd vecklinje (t.ex. från en annan instans) till den här instansens residual.

    Söker residualpixlar inom ±search_mm från den förväntade linjen och anpassar en linje
    med PCA. Saknas tillräckligt stöd behålls den förväntade linjen.
    """
    h, w = ref_L.shape
    ds = max(1, int(round(ppm / 6)))
    r = cv2.resize(ref_L, (w // ds, h // ds), interpolation=cv2.INTER_AREA)
    s = cv2.resize(smp_L, (w // ds, h // ds), interpolation=cv2.INTER_AREA)
    v = cv2.resize(valid.astype(np.uint8), (w // ds, h // ds), interpolation=cv2.INTER_NEAREST).astype(bool)
    res = cv2.GaussianBlur(s - r, (0, 0), 1.0)
    res = res - cv2.GaussianBlur(res, (0, 0), 12)
    mag = np.abs(res)
    edges_ref = cv2.dilate((cv2.Canny(np.clip(r * 2.55, 0, 255).astype(np.uint8), 30, 90) > 0).astype(np.uint8),
                           np.ones((3, 3), np.uint8)) > 0
    flat = v & ~edges_ref
    sigma = 1.4826 * float(np.median(mag[flat])) if flat.any() else 1.0
    t_line = max(2.0, 3.0 * sigma)
    yy, xx = np.nonzero((mag > t_line) & flat)
    if len(xx) < 30:
        return prior
    pts = np.stack([xx, yy], 1).astype(np.float64)
    a0 = np.array(prior.p0, float) / ds
    d = np.array(prior.p1, float) / ds - a0
    d /= max(np.linalg.norm(d), 1e-9)
    n = np.array([-d[1], d[0]])
    lim = search_mm * ppm / ds
    for it in range(4):
        near = pts[np.abs((pts - a0) @ n) < (lim if it == 0 else 3.0)]
        if len(near) < 30:
            return prior
        mu = near.mean(axis=0)
        _, _, vt = np.linalg.svd(near - mu, full_matrices=False)
        # håll vinkeln nära den förväntade (veck är raka över hela arket)
        if abs(float(np.dot(vt[0], d))) < 0.995:
            a0 = mu
            continue
        d = vt[0] if np.dot(vt[0], d) > 0 else -vt[0]
        n = np.array([-d[1], d[0]])
        a0 = mu
    L = 2 * (w + h) / ds
    return Fold(tuple((a0 - d * L) * ds), tuple((a0 + d * L) * ds), prior.band_px)
