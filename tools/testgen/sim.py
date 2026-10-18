"""Tryck- och scansimulering (avsnitt 9.4). Allt slumpas från en given rng."""
from __future__ import annotations

import cv2
import numpy as np

PAPER = np.array([0.955, 0.950, 0.925], np.float32)
ANGLES = {"C": 15.0, "M": 75.0, "Y": 0.0, "K": 45.0}


def rgb_to_cmyk(rgb):
    k = 1.0 - rgb.max(axis=2)
    d = np.maximum(1.0 - k, 1e-4)
    c = (1.0 - rgb[..., 0] - k) / d
    m = (1.0 - rgb[..., 1] - k) / d
    y = (1.0 - rgb[..., 2] - k) / d
    return {"C": c, "M": m, "Y": y, "K": k}


def halftone(t, dpi, lpcm, angle_deg, phase=(0.0, 0.0)):
    h, w = t.shape
    period = dpi / (lpcm * 2.54)
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    a = np.deg2rad(angle_deg)
    u = (xx * np.cos(a) + yy * np.sin(a)) / period + phase[0]
    v = (-xx * np.sin(a) + yy * np.cos(a)) / period + phase[1]
    spot = 0.04 + 0.92 * (0.5 + 0.25 * (np.cos(2 * np.pi * u) + np.cos(2 * np.pi * v)))
    # mjuk tröskel ger kantutjämnade punkter (motsvarar plåtens upplösning);
    # tonplatta 100 % blir helt täckt, 0 % helt fri.
    return np.clip((t - spot) * 12 + 0.5, 0, 1).astype(np.float32)


def dot_gain(t, g):
    return np.clip(t + 4 * g * t * (1 - t), 0, 1)


def combine(plates, paper=PAPER):
    c, m, y, k = (plates[n] for n in "CMYK")
    r = (1 - c) * (1 - k)
    g = (1 - m) * (1 - k)
    b = (1 - y) * (1 - k)
    return np.stack([r, g, b], -1) * paper


def print_label(rgb_hi_u8, dpi_hi, dpi_out, p, rng):
    """Simulera tryck av en etikett. Returnerar float32 RGB vid dpi_out."""
    rgb = rgb_hi_u8.astype(np.float32) / 255.0
    plates = rgb_to_cmyk(rgb)
    out = {}
    for n, t in plates.items():
        t = dot_gain(t * p["density"][n], p["dot_gain"][n])
        dx, dy = (v * dpi_hi / 25.4 for v in p["misreg_mm"][n])
        if abs(dx) > 1e-3 or abs(dy) > 1e-3:
            M = np.float32([[1, 0, dx], [0, 1, dy]])
            t = cv2.warpAffine(t, M, (t.shape[1], t.shape[0]), flags=cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
        if p["halftone"]:
            t = halftone(t, dpi_hi, p["lpcm"], ANGLES[n], phase=tuple(rng.uniform(0, 1, 2)))
        out[n] = t
    img = combine(out)
    if p["halftone"]:
        # Pressen antas vara kalibrerad mot provtrycket: medelfärgen (lågpass) ska ligga nära
        # PDF:ens färg med en kontrollerad tonavvikelse, medan rastret, passningsfel och
        # kantstruktur behålls. Utan detta ger den naiva CMYK-modellen orealistiska färgfel.
        gam = 1.0 + np.array([p["dot_gain"]["C"], p["dot_gain"]["M"], p["dot_gain"]["Y"]], np.float32) * 0.6
        target = PAPER * np.power(np.clip(rgb, 1e-4, 1.0), gam)
        sig = 1.5 * dpi_hi / (p["lpcm"] * 2.54)
        bt = cv2.GaussianBlur(target, (0, 0), sig)
        bi = cv2.GaussianBlur(img, (0, 0), sig)
        corr = np.clip(bt / np.maximum(bi, 1e-3), 0.2, 5.0)
        img = np.clip(img * corr, 0, 1)
    s = dpi_out / dpi_hi
    return cv2.resize(img, None, fx=s, fy=s, interpolation=cv2.INTER_AREA)


def add_print_defect(img, ppm, kind, rng, region_mm, avoid_mm=()):
    """Lägg till ett lokalt tryckfel. Returnerar facitruta i mm eller None."""
    h, w = img.shape[:2]
    x0r, y0r, x1r, y1r = region_mm
    for _ in range(30):
        if kind == "spot":
            rx, ry = rng.uniform(0.12, 0.8, 2)
        elif kind == "streak":
            rx, ry = rng.uniform(0.05, 0.15), rng.uniform(2.0, 9.0)
        else:  # hickey
            rx = ry = rng.uniform(0.35, 0.9)
        cx, cy = rng.uniform(x0r + rx + 0.5, x1r - rx - 0.5), rng.uniform(y0r + ry + 0.5, y1r - ry - 0.5)
        box = [cx - rx, cy - ry, 2 * rx, 2 * ry]
        if any(not (box[0] > a[0] + a[2] + 1 or a[0] > box[0] + box[2] + 1 or
                    box[1] > a[1] + a[3] + 1 or a[1] > box[1] + box[3] + 1) for a in avoid_mm):
            continue
        m = np.zeros((h, w), np.float32)
        center = (int(cx * ppm * 16), int(cy * ppm * 16))
        axes = (max(1, int(rx * ppm * 16)), max(1, int(ry * ppm * 16)))
        if kind == "hickey":
            cv2.ellipse(m, center, axes, 0, 0, 360, 1.0, thickness=max(16, int(0.12 * ppm * 16)), lineType=cv2.LINE_AA, shift=4)
            cv2.circle(m, center, max(1, int(0.12 * ppm * 16)), 1.0, -1, cv2.LINE_AA, shift=4)
        else:
            cv2.ellipse(m, center, axes, 0, 0, 360, 1.0, -1, cv2.LINE_AA, shift=4)
        m = cv2.GaussianBlur(m, (0, 0), 0.6)
        dark = kind != "spot" or rng.random() < 0.6
        if dark:
            col = np.array([0.18, 0.17, 0.2], np.float32) if rng.random() < 0.7 else np.array([0.1, 0.25, 0.55], np.float32)
            strength = rng.uniform(0.55, 0.95)
            img[:] = img * (1 - strength * m[..., None]) + col * (strength * m[..., None])
            polarity = "dark"
        else:
            strength = rng.uniform(0.6, 0.95)
            img[:] = img * (1 - strength * m[..., None]) + PAPER * (strength * m[..., None])
            polarity = "light"
        return box, polarity, float(strength)
    return None


def rounded_rect_mask(w_px, h_px, r_px):
    m = np.zeros((h_px, w_px), np.uint8)
    r = int(r_px)
    cv2.rectangle(m, (r, 0), (w_px - 1 - r, h_px - 1), 1, -1)
    cv2.rectangle(m, (0, r), (w_px - 1, h_px - 1 - r), 1, -1)
    for cx, cy in ((r, r), (w_px - 1 - r, r), (r, h_px - 1 - r), (w_px - 1 - r, h_px - 1 - r)):
        cv2.circle(m, (cx, cy), r, 1, -1, cv2.LINE_AA)
    return m.astype(bool)


def fold(img, p0, p1, w_px, strength, amp_px):
    """Veck längs linjen p0-p1: förskjutning vinkelrätt mot linjen + skuggband."""
    h, wd = img.shape[:2]
    yy, xx = np.mgrid[0:h, 0:wd].astype(np.float32)
    dx, dy = p1[0] - p0[0], p1[1] - p0[1]
    n = np.hypot(dx, dy)
    nx, ny = -dy / n, dx / n
    s = (xx - p0[0]) * nx + (yy - p0[1]) * ny
    d = amp_px * np.tanh(s / (3 * w_px))
    mx, my = (xx - d * nx).astype(np.float32), (yy - d * ny).astype(np.float32)
    del xx, yy
    out = cv2.remap(img, mx, my, cv2.INTER_CUBIC, borderMode=cv2.BORDER_REPLICATE)
    del mx, my, d
    shade = (1 - strength * np.exp(-(s / w_px) ** 2) * np.sign(s)
             - 0.35 * strength * np.exp(-(s / (0.4 * w_px)) ** 2)).astype(np.float32)
    out *= shade[..., None]
    return np.clip(out, 0, 1, out=out)


def scanner(img, rng, mtf_sigma, noise_sigma, color_jitter=0.012, gamma=1.0):
    out = cv2.GaussianBlur(img, (0, 0), sigmaX=mtf_sigma[0], sigmaY=mtf_sigma[1])
    M = np.eye(3, dtype=np.float32) + rng.normal(0, color_jitter, (3, 3)).astype(np.float32)
    out = np.clip(out @ M.T, 0, 1) ** gamma
    noisy = out * 255 + rng.normal(0, noise_sigma, out.shape).astype(np.float32)
    return np.clip(noisy + 0.5, 0, 255).astype(np.uint8)
