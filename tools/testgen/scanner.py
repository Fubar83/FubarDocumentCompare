"""Flatbäddsscanner och fysiska veck (simulator v2).

Två sensortyper:

* CIS (contact image sensor): linsrad direkt under glaset, LED-ljus från en sida.
  Mycket kort skärpedjup och snabbt ljusfall när papperet lyfter. Ingen perspektivverkan.
  RGB-dioderna blinkar i tur och ordning, vilket ger en liten kanalförskjutning i
  matningsriktningen.
* CCD: lampa och spegel/lins-system med lång optisk väg. Långt skärpedjup och svagt
  ljusfall, men centralprojektion tvärs vagnen: lyft innehåll förskjuts radiellt från
  optiska axeln. Trilinjär sensor med liten kvarvarande kanalförskjutning.

Bildkoordinater: x tvärs vagnen (sensorraden), y i matningsriktningen.

Veck modelleras som en höjdkarta h(x, y) i mm över glaset (lock stängt, papperet
delvis nedtryckt). Av höjden följer:

* oskärpa (sigma ökar med h, mycket mer för CIS),
* ljusfall (belysningen avtar med h),
* sluttningsskuggning (ljuset kommer snett i matningsriktningen),
* förkortning (papperets båglängd i det lyfta bandet projiceras kortare),
* perspektiv (bara CCD).

Ett bergveck (crease uppåt) kan dessutom spräcka färgen på heltäckande ytor: en
tunn ljus linje längs vecket.
"""
from __future__ import annotations

from dataclasses import dataclass, field

import cv2
import numpy as np


@dataclass
class ScannerModel:
    kind: str                       # "cis" | "ccd"
    blur_px_per_mm: float           # extra oskärpa (sigma, px vid 600 dpi) per mm lyft
    light_h0_mm: float              # ljusfall: I = 1 / (1 + (h / h0)^2)
    slope_gain: float               # skuggning: 1 + gain * dh/dy (ljus från +y)
    light_sign: float               # +1: ljus från +y, -1: från -y
    persp_L_mm: float               # CCD: effektiv projektionsavstånd; 0 = ingen perspektivverkan
    chan_shift_px: tuple            # R, G, B förskjutning i y (px)
    jitter_px: float                # stegmotor: lågfrekvent y-fel (amplitud)
    y_scale: float                  # matningshastighetens skalfel
    vignette: float                 # relativt ljusfall vid sensorradens ände
    shot: float                     # signalberoende brus (varians per gråvärde)
    gamma: float
    emboss_gain: float              # relativ präglingssignal (låg ljusvinkel = mer skugga)
    params: dict = field(default_factory=dict)


def make_model(kind: str, rng: np.random.Generator) -> ScannerModel:
    if kind == "cis":
        d = float(rng.uniform(0.25, 0.4))
        return ScannerModel(
            kind="cis", blur_px_per_mm=float(rng.uniform(3.0, 5.0)), light_h0_mm=float(rng.uniform(1.2, 1.8)),
            slope_gain=float(rng.uniform(0.4, 0.7)), light_sign=1.0 if rng.random() < 0.5 else -1.0,
            persp_L_mm=0.0, chan_shift_px=(-d, 0.0, d), jitter_px=float(rng.uniform(0.05, 0.15)),
            y_scale=float(1 + rng.uniform(-0.002, 0.002)), vignette=float(rng.uniform(0.0, 0.015)),
            shot=float(rng.uniform(0.02, 0.05)), gamma=float(rng.uniform(0.93, 1.07)),
            emboss_gain=float(rng.uniform(0.7, 0.9)))
    if kind == "ccd":
        return ScannerModel(
            kind="ccd", blur_px_per_mm=float(rng.uniform(0.3, 0.7)), light_h0_mm=float(rng.uniform(6.0, 10.0)),
            slope_gain=float(rng.uniform(0.7, 1.1)), light_sign=1.0 if rng.random() < 0.5 else -1.0,
            persp_L_mm=float(rng.uniform(250, 400)),
            chan_shift_px=tuple(float(v) for v in rng.uniform(-0.15, 0.15, 3)),
            jitter_px=float(rng.uniform(0.15, 0.35)), y_scale=float(1 + rng.uniform(-0.003, 0.003)),
            vignette=float(rng.uniform(0.015, 0.04)), shot=float(rng.uniform(0.01, 0.03)),
            gamma=float(rng.uniform(0.93, 1.07)), emboss_gain=float(rng.uniform(1.1, 1.4)))
    raise ValueError(kind)


# ------------------------------------------------------------------ veck

@dataclass
class Fold:
    p0: tuple                       # px i scannerns bild (linjen)
    p1: tuple
    kind: str                       # "mountain" | "valley"
    lift_mm: float                  # största höjd över glaset
    width_mm: float                 # avklingningslängd
    crack: bool = False
    crack_w_mm: float = 0.08

    def normal(self):
        dx, dy = self.p1[0] - self.p0[0], self.p1[1] - self.p0[1]
        n = np.hypot(dx, dy)
        return -dy / n, dx / n

    def signed_dist(self, x, y):
        nx, ny = self.normal()
        return (x - self.p0[0]) * nx + (y - self.p0[1]) * ny

    def profile(self, s_mm):
        """Höjd (mm) och derivata dh/ds som funktion av avstånd till veckets linje."""
        a = np.abs(s_mm) / self.width_mm
        if self.kind == "mountain":
            h = self.lift_mm * np.exp(-a)
            dh = -np.sign(s_mm) * h / self.width_mm
        else:
            # dalveck: vecklinjen ligger mot glaset, vingarna reser sig och pressas ned igen
            h = self.lift_mm * a * np.exp(1 - a)
            dh = np.sign(s_mm) * self.lift_mm * (1 - a) * np.exp(1 - a) / self.width_mm
        return h, dh

    def arc_excess(self, s_mm):
        """Hur mycket längre papperet är än projektionen från linjen ut till s (mm, med tecken)."""
        # integrera sqrt(1 + h'^2) - 1 numeriskt på en fast tabell och interpolera
        t = np.linspace(0, 8 * self.width_mm, 1024)
        _, dh = self.profile(t)
        ex = np.concatenate([[0], np.cumsum((np.sqrt(1 + dh[1:] ** 2) - 1) * np.diff(t))])
        return np.sign(s_mm) * np.interp(np.abs(s_mm), t, ex)

    def band_mm(self, h_min: float = 0.05) -> float:
        """Halv bredd av zonen där papperet lyfter mer än h_min."""
        if self.lift_mm <= h_min:
            return 0.5
        if self.kind == "mountain":
            return float(self.width_mm * np.log(self.lift_mm / h_min))
        a = np.linspace(0, 20, 4000)
        hh = self.lift_mm * a * np.exp(1 - a)
        return float(a[np.nonzero(hh > h_min)[0].max()] * self.width_mm)


def random_fold(rng, SW, SH, ppm, lift_range=(0.1, 1.2), crack_p=0.4) -> Fold:
    vertical = rng.random() < 0.5
    a = np.deg2rad(rng.uniform(-3, 3))
    if vertical:
        x = rng.uniform(0.3, 0.7) * SW
        p0 = (x - np.tan(a) * SH / 2, 0.0)
        p1 = (x + np.tan(a) * SH / 2, float(SH))
    else:
        y = rng.uniform(0.3, 0.7) * SH
        p0 = (0.0, y - np.tan(a) * SW / 2)
        p1 = (float(SW), y + np.tan(a) * SW / 2)
    kind = "mountain" if rng.random() < 0.7 else "valley"
    lift = float(rng.uniform(*lift_range))
    return Fold((float(p0[0]), float(p0[1])), (float(p1[0]), float(p1[1])), kind, lift,
                float(rng.uniform(1.5, 5.0)), crack=bool(kind == "mountain" and rng.random() < crack_p),
                crack_w_mm=float(rng.uniform(0.05, 0.15)))


def crack_ink(sheet: np.ndarray, G: np.ndarray, fold: Fold, ppm: float, rng, paper) -> None:
    """Spräckt färg längs ett bergveck (i arkets koordinater, före scanning)."""
    Gi = cv2.invertAffineTransform(G)
    p0 = Gi @ np.array([*fold.p0, 1.0])
    p1 = Gi @ np.array([*fold.p1, 1.0])
    h, w = sheet.shape[:2]
    m = np.zeros((h, w), np.float32)
    n = int(np.hypot(*(p1 - p0)) / (0.5 * ppm)) + 2
    # sprickan är bruten: slumpvisa partier
    on = cv2.resize(rng.random((1, n // 6 + 2)).astype(np.float32), (n, 1), interpolation=cv2.INTER_LINEAR)[0] > 0.45
    th = max(1, int(round(fold.crack_w_mm * ppm)))
    for i in range(n - 1):
        if not on[i]:
            continue
        a = p0 + (p1 - p0) * i / (n - 1)
        b = p0 + (p1 - p0) * (i + 1) / (n - 1)
        cv2.line(m, (int(a[0] * 16), int(a[1] * 16)), (int(b[0] * 16), int(b[1] * 16)), 1.0, th, cv2.LINE_AA, shift=4)
    m = cv2.GaussianBlur(m, (0, 0), 0.5)[..., None]
    # bara där det finns färg (mörkt) syns sprickan
    ink = np.clip((paper.mean() - sheet.mean(axis=2, keepdims=True)) / 0.5, 0, 1)
    sheet[:] = sheet * (1 - 0.85 * m * ink) + paper * (0.85 * m * ink)


# ------------------------------------------------------------------ scanning

class Geometry:
    """Avbildning scannerbild (q) -> ark (p), och höjd över glaset."""

    def __init__(self, G: np.ndarray, folds: list[Fold], model: ScannerModel, ppm: float, size, rng):
        self.Gi = cv2.invertAffineTransform(G)
        self.folds, self.model, self.ppm = folds, model, ppm
        self.SW, self.SH = size
        # stegmotor: summa av några sinusar (perioder 3–40 mm)
        k = 5
        self.jit = [(float(rng.uniform(3, 40) * ppm), float(rng.uniform(0, 2 * np.pi)),
                     float(model.jitter_px * rng.uniform(0.3, 1.0) / np.sqrt(k))) for _ in range(k)]

    def height(self, x, y):
        H = np.zeros(np.broadcast(x, y).shape, np.float32)
        dHy = np.zeros_like(H)
        for f in self.folds:
            s = f.signed_dist(x, y) / self.ppm
            h, dh = f.profile(s)
            H += h.astype(np.float32)
            dHy += (dh * f.normal()[1]).astype(np.float32)
        return H, dHy

    def to_sheet(self, x, y, H=None):
        """Inversa avbildningen: scannerbildens pixel -> arkets pixel."""
        x = np.asarray(x, np.float64)
        y = np.asarray(y, np.float64)
        if H is None:
            H, _ = self.height(x, y)
        # stegmotor och skalfel i matningsriktningen
        yy = y / self.model.y_scale
        for per, ph, amp in self.jit:
            yy = yy - amp * np.sin(2 * np.pi * y / per + ph)
        xx = x.copy()
        # CCD: lyft innehåll syns förskjutet från optiska axeln -> dra tillbaka
        if self.model.persp_L_mm > 0:
            xc = self.SW / 2
            xx = xx - H * (x - xc) / self.model.persp_L_mm
        # förkortning: punkten vid projicerat avstånd s ligger på papperet vid s + överskott
        for f in self.folds:
            s = f.signed_dist(x, y) / self.ppm
            ex = f.arc_excess(s) * self.ppm
            nx, ny = f.normal()
            xx = xx + ex * nx
            yy = yy + ex * ny
        px = self.Gi[0, 0] * xx + self.Gi[0, 1] * yy + self.Gi[0, 2]
        py = self.Gi[1, 0] * xx + self.Gi[1, 1] * yy + self.Gi[1, 2]
        return px, py

    def to_scan(self, px, py, G: np.ndarray, iters: int = 6):
        """Framåt: ark -> scannerbild, genom iterativ invertering av to_sheet."""
        px = np.asarray(px, np.float64)
        py = np.asarray(py, np.float64)
        qx = G[0, 0] * px + G[0, 1] * py + G[0, 2]
        qy = G[1, 0] * px + G[1, 1] * py + G[1, 2]
        for _ in range(iters):
            ex, ey = self.to_sheet(qx, qy)
            rx, ry = px - ex, py - ey
            qx = qx + G[0, 0] * rx + G[0, 1] * ry
            qy = qy + G[1, 0] * rx + G[1, 1] * ry
        return qx, qy


def _varying_blur(img: np.ndarray, sigma: np.ndarray, levels=(0.0, 0.7, 1.4, 2.8, 5.6, 11.2)) -> np.ndarray:
    """Rumsligt varierande gaussisk oskärpa genom interpolation mellan nivåer."""
    smax = float(sigma.max())
    lv = [l for l in levels if l <= smax * 1.01] + [l for l in levels if l > smax][:1]
    if len(lv) < 2:
        return img
    stack = [img if l == 0 else cv2.GaussianBlur(img, (0, 0), l) for l in lv]
    out = np.zeros_like(img)
    lv = np.array(lv, np.float32)
    s = np.clip(sigma, 0, lv[-1])
    for i in range(len(lv) - 1):
        a, b = lv[i], lv[i + 1]
        sel = (s >= a) & (s <= b) if i == 0 else (s > a) & (s <= b)
        if not sel.any():
            continue
        t = ((s - a) / (b - a))[..., None]
        blend = stack[i] * (1 - t) + stack[i + 1] * t
        out[sel] = blend[sel]
    return out


def scan(sheet: np.ndarray, G: np.ndarray, folds: list[Fold], model: ScannerModel, rng, ppm: float,
         mtf_sigma, read_noise: float, color_jitter: float = 0.012, lid=(0.97, 0.97, 0.97),
         dust_n: int | None = None):
    """Scanna ett ark. Returnerar (uint8-bild, Geometry, lista med dammfläckar i scanpx)."""
    SH, SW = sheet.shape[:2]
    geo = Geometry(G, folds, model, ppm, (SW, SH), rng)
    chans = [np.ascontiguousarray(sheet[..., c]) for c in range(3)]
    out = np.empty((SH, SW, 3), np.float32)
    Hmap = np.zeros((SH, SW), np.float32) if folds else None
    shade = np.ones((SH, SW), np.float32) if folds else None
    xs = np.arange(SW, dtype=np.float64)
    step = 256
    for y0 in range(0, SH, step):
        y1 = min(SH, y0 + step)
        yy, xx = np.mgrid[y0:y1, 0:SW].astype(np.float64)
        H, dHy = geo.height(xx, yy) if folds else (None, None)
        px, py = geo.to_sheet(xx, yy, H)
        for c in range(3):
            # kanalförskjutning i matningsriktningen (RGB exponeras vid olika vagnläge)
            dy = model.chan_shift_px[c]
            py_c = (py + dy * geo.Gi[1, 1]).astype(np.float32)
            px_c = (px + dy * geo.Gi[0, 1]).astype(np.float32)
            out[y0:y1, :, c] = cv2.remap(chans[c], px_c, py_c, cv2.INTER_CUBIC, borderMode=cv2.BORDER_CONSTANT,
                                         borderValue=float(lid[c]))
        if folds:
            Hmap[y0:y1] = H
            fall = 1.0 / (1.0 + (H / model.light_h0_mm) ** 2)
            sl = 1.0 + model.slope_gain * model.light_sign * dHy
            shade[y0:y1] = np.clip(fall * sl, 0.05, 1.6)
    del chans
    if folds:
        # oskärpa efter höjd, bara i området där papperet lyfter
        sig = Hmap * model.blur_px_per_mm
        ys, xs_ = np.nonzero(sig > 0.15)
        if len(ys):
            pad = int(4 * sig.max()) + 4
            a0, a1 = max(0, ys.min() - pad), min(SH, ys.max() + pad + 1)
            b0, b1 = max(0, xs_.min() - pad), min(SW, xs_.max() + pad + 1)
            out[a0:a1, b0:b1] = _varying_blur(out[a0:a1, b0:b1], sig[a0:a1, b0:b1])
        out *= shade[..., None]
        del sig, shade
    # damm på glaset (i fokus, mörkt)
    dust = []
    n_d = int(rng.poisson(1.5)) if dust_n is None else dust_n
    for _ in range(n_d):
        cx, cy = float(rng.uniform(0.05, 0.95) * SW), float(rng.uniform(0.05, 0.95) * SH)
        r = float(rng.uniform(0.03, 0.12) * ppm)
        m = np.zeros((int(4 * r) + 3, int(4 * r) + 3), np.float32)
        cv2.ellipse(m, ((m.shape[1] // 2) * 16, (m.shape[0] // 2) * 16),
                    (int(r * 16), int(r * rng.uniform(0.5, 1.0) * 16)), float(rng.uniform(0, 180)), 0, 360, 1.0, -1,
                    cv2.LINE_AA, shift=4)
        x0, y0 = int(cx) - m.shape[1] // 2, int(cy) - m.shape[0] // 2
        sub = out[y0:y0 + m.shape[0], x0:x0 + m.shape[1]]
        mm = m[:sub.shape[0], :sub.shape[1], None] * 0.8
        sub[:] = sub * (1 - mm) + 0.15 * mm
        dust.append([x0, y0, m.shape[1], m.shape[0]])
    # optikens grundskärpa (kan skilja i x och y)
    out = cv2.GaussianBlur(out, (0, 0), sigmaX=mtf_sigma[0], sigmaY=mtf_sigma[1])
    # vinjettering tvärs sensorraden
    if model.vignette > 0:
        v = 1 - model.vignette * ((xs - SW / 2) / (SW / 2)) ** 2
        out *= v.astype(np.float32)[None, :, None]
    # färgprofil och tonkurva
    M = np.eye(3, dtype=np.float32) + rng.normal(0, color_jitter, (3, 3)).astype(np.float32)
    out = np.clip(out.reshape(-1, 3) @ M.T, 0, 1).reshape(out.shape)
    np.power(out, model.gamma, out=out)
    out *= 255.0
    # brus: läsbrus + signalberoende (fotonbrus)
    sd = np.sqrt(read_noise ** 2 + model.shot * out)
    out += rng.standard_normal(out.shape, dtype=np.float32) * sd
    del sd
    np.clip(out + 0.5, 0, 255, out=out)
    return out.astype(np.uint8), geo, dust
