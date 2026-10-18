"""Normalisering (avsnitt 6.4), körs efter registrering i referensens ram.

- rasterdämpning: samma lågpass på referens och prov,
- belysning: lågfrekvent kvot mellan prov och referens divideras bort (robust),
- färg: robust affin färgkorrigering 3x4 (prov -> referens) skattad över hela etiketten.

Färgkorrigeringen är en "självkalibrering" för prototypen. I produkten ersätts den av
scannerprofil (IT8) + pappersvitt, så att globala färgfel inte kalibreras bort.
"""
from __future__ import annotations

import cv2
import numpy as np


def to_float(rgb: np.ndarray) -> np.ndarray:
    return rgb.astype(np.float32) / 255.0


def estimate_screen_period_px(sample_gray: np.ndarray, ref_gray: np.ndarray, valid: np.ndarray,
                              n: int = 128) -> float | None:
    """Skatta rasterperioden ur spektrumet i tonplattor.

    Vi letar ytor där referensen är jämn men tryckt (en tonplatta). Där ger ett
    raster tydliga spektraltoppar, medan text och streckkoder inte stör.
    """
    h, w = ref_gray.shape
    if min(h, w) < n:
        return None
    win = np.outer(np.hanning(n), np.hanning(n)).astype(np.float32)
    yy, xx = np.mgrid[0:n, 0:n]
    r = np.hypot(yy - n // 2, xx - n // 2)
    band = (r > n / 14) & (r < n / 2.3)               # period ca 2.3..14 px
    found = []
    for y0 in range(0, h - n + 1, n // 2):
        for x0 in range(0, w - n + 1, n // 2):
            if not valid[y0:y0 + n, x0:x0 + n].all():
                continue
            rt = ref_gray[y0:y0 + n, x0:x0 + n]
            if rt.std() > 0.015 or not (0.08 < rt.mean() < 0.9):
                continue
            tile = sample_gray[y0:y0 + n, x0:x0 + n].astype(np.float32)
            tile = (tile - tile.mean()) * win
            F = np.abs(np.fft.fftshift(np.fft.fft2(tile)))
            idx = np.argmax(F * band)
            strength = F.flat[idx] / (np.median(F[band]) + 1e-9)
            if strength > 12:
                found.append((n / r.flat[idx], strength))
            if len(found) >= 6:
                break
        if len(found) >= 6:
            break
    if not found:
        return None
    return float(np.median([p for p, _ in found]))


def estimate_extra_blur(ref_f: np.ndarray, smp_f: np.ndarray, valid: np.ndarray,
                        sigmas=(0.0, 0.3, 0.5, 0.7, 0.9, 1.1, 1.3, 1.6, 2.0)) -> float:
    """Skatta hur mycket mer oskarpt provet är än referensen (scannerns MTF m.m.).

    Referensen suddas med varje kandidat-sigma; den som ger högst korrelation mellan
    Laplace-svaren i kantområden vinner. Tunna streck (streckkoder, liten text) tappar
    annars kontrast i provet och tolkas som saknade.
    """
    g = lambda x: 0.299 * x[..., 0] + 0.587 * x[..., 1] + 0.114 * x[..., 2]
    gr, gs = g(ref_f).astype(np.float32), g(smp_f).astype(np.float32)
    edge = valid & (cv2.morphologyEx(gr, cv2.MORPH_GRADIENT, np.ones((3, 3), np.uint8)) > 0.1)
    edge = cv2.dilate(edge.astype(np.uint8), np.ones((5, 5), np.uint8)).astype(bool) & valid
    if edge.sum() < 500:
        return 0.0
    ls = cv2.Laplacian(gs, cv2.CV_32F, ksize=3)[edge]
    ls = (ls - ls.mean()) / (ls.std() + 1e-9)
    best, best_s = -2.0, 0.0
    for s in sigmas:
        b = cv2.GaussianBlur(gr, (0, 0), s) if s > 0 else gr
        lr = cv2.Laplacian(b, cv2.CV_32F, ksize=3)[edge]
        lr = (lr - lr.mean()) / (lr.std() + 1e-9)
        ncc = float(np.mean(lr * ls))
        if ncc > best:
            best, best_s = ncc, s
    return best_s


def descreen(img_f: np.ndarray, sigma_px: float) -> np.ndarray:
    if sigma_px <= 0.3:
        return img_f
    return cv2.GaussianBlur(img_f, (0, 0), sigma_px)


def _luma(img_f):
    return 0.299 * img_f[..., 0] + 0.587 * img_f[..., 1] + 0.114 * img_f[..., 2]


def flatten_illumination(sample_f: np.ndarray, ref_f: np.ndarray, valid: np.ndarray, ppm: float,
                         sigma_mm: float = 8.0) -> tuple[np.ndarray, np.ndarray]:
    """Dela bort lågfrekvent belysningsskillnad. Returnerar (korrigerat prov, kvotkarta)."""
    ds = max(1, int(round(ppm * 0.5)))                 # ~0,5 mm per cell
    ys = _luma(sample_f)[::ds, ::ds]
    yr = _luma(ref_f)[::ds, ::ds]
    v = valid[::ds, ::ds] & (yr > 0.25)
    ratio = np.where(v, ys / np.maximum(yr, 1e-3), np.nan).astype(np.float32)
    # robust: median i stort fönster ignorerar lokala fel, sedan utfyllnad + utjämning
    filled = np.where(np.isnan(ratio), np.nanmedian(ratio) if np.isfinite(ratio).any() else 1.0, ratio)
    med =cv2.medianBlur(np.clip(filled, 0, 4).astype(np.float32), 5)
    sm = cv2.GaussianBlur(med, (0, 0), sigma_mm / 0.5 / 2)
    full = cv2.resize(sm, (sample_f.shape[1], sample_f.shape[0]), interpolation=cv2.INTER_LINEAR)
    full = np.clip(full, 0.5, 2.0)
    return np.clip(sample_f / full[..., None], 0, 1), full


def _features(x: np.ndarray) -> np.ndarray:
    """RGB -> [r, g, b, 1, r², g², b², rg, rb, gb]: affint plus andragradstermer."""
    r, g, b = x[..., 0], x[..., 1], x[..., 2]
    one = np.ones_like(r)
    return np.stack([r, g, b, one, r * r, g * g, b * b, r * g, r * b, g * b], axis=-1)


def fit_color(sample_f: np.ndarray, ref_f: np.ndarray, valid: np.ndarray, iters: int = 6) -> dict:
    """Robust färgmappning prov -> referens: andragradspolynom i RGB med ridge.

    - affina termer regulariseras lätt mot identitet, andragradstermer mot noll, så att
      etiketter med få färger inte ger en vild avbildning;
    - andragradstermerna fångar tonkurvor (punktförstoring, gamma) och korskanaler;
    - jämna ytor i båda bilderna används (inkl. insidan av text, så svärtan kalibreras);
    - lokala fel blir utliggare (Huber-vikter).
    """
    step = 3
    g = 0.299 * ref_f[..., 0] + 0.587 * ref_f[..., 1] + 0.114 * ref_f[..., 2]
    gs = 0.299 * sample_f[..., 0] + 0.587 * sample_f[..., 1] + 0.114 * sample_f[..., 2]
    k = np.ones((3, 3), np.uint8)
    m = valid & (cv2.morphologyEx(g, cv2.MORPH_GRADIENT, k) < 0.08) & \
        (cv2.morphologyEx(gs, cv2.MORPH_GRADIENT, k) < 0.08)
    s = sample_f[::step, ::step][m[::step, ::step]].astype(np.float64)
    r = ref_f[::step, ::step][m[::step, ::step]].astype(np.float64)
    B0 = np.zeros((10, 3))
    B0[0, 0] = B0[1, 1] = B0[2, 2] = 1.0
    if len(s) < 200:
        return {"B": B0.astype(np.float32), "M": B0[:4].T.astype(np.float32)}
    if len(s) > 200000:
        sel = np.random.default_rng(0).choice(len(s), 200000, replace=False)
        s, r = s[sel], r[sel]
    # Ankarpunkt för svärta: mörkaste percentilen i respektive bild ska mappas på varandra.
    # Annars kan andragradstermerna extrapolera fel när svarta ytor är få eller tunna (text).
    lum_s = 0.299 * sample_f[..., 0] + 0.587 * sample_f[..., 1] + 0.114 * sample_f[..., 2]
    anchor = None
    vs = valid[::2, ::2]
    ls, lr = lum_s[::2, ::2][vs], g[::2, ::2][vs]
    if ls.size > 1000:
        qs, qr = np.percentile(ls, 0.3), np.percentile(lr, 0.3)
        if qr < 0.25:                                    # referensen har verklig svärta
            s_black = sample_f[::2, ::2][vs][ls <= qs].mean(axis=0)
            r_black = ref_f[::2, ::2][vs][lr <= qr].mean(axis=0)
            anchor = (s_black, r_black)
    X = _features(s)
    wts = np.ones(len(s))
    B = B0
    if anchor is not None:
        # ankaret har fast vikt (10 %) och omfattas inte av Huber-nedviktningen
        Xa = _features(anchor[0][None, :])
        ra = anchor[1][None, :]
    for _ in range(iters):
        sw = np.sqrt(wts)[:, None]
        n_eff = wts.sum()
        lam = np.array([0.003, 0.003, 0.003, 0.003, 0.01, 0.01, 0.01, 0.01, 0.01, 0.01]) * n_eff
        A = (X * sw).T @ (X * sw) + np.diag(lam)
        Bm = (X * sw).T @ (r * sw) + lam[:, None] * B0
        if anchor is not None:
            wa = 0.10 * n_eff
            A = A + wa * Xa.T @ Xa
            Bm = Bm + wa * Xa.T @ ra
        B = np.linalg.solve(A, Bm)
        res = np.linalg.norm(X @ B - r, axis=1)
        # Huber-gräns: minst 0,06 (≈ ΔE 5–6) – annars blir allt utom papperet "utliggare"
        # när vitt dominerar etiketten, och färgytorna slutar påverka anpassningen
        c = max(2.0 * float(np.median(res)), 0.06)
        wts = np.where(res < c, 1.0, c / np.maximum(res, 1e-9))
    return {"B": B.astype(np.float32), "M": B[:4].T.astype(np.float32)}


def apply_color(sample_f: np.ndarray, model: dict) -> np.ndarray:
    out = _features(sample_f.astype(np.float32)) @ model["B"]
    return np.clip(out, 0, 1).astype(np.float32)
