"""Registrering (avsnitt 6.5): global homografi via faskorrelation i tiles + RANSAC,
lokal deformation via tätt flöde som regulariseras till ett grovt nät, och en enda
omsampling från originalscanningen.

Konvention: H mappar referens-pixel -> prov-pixel (3x3).
Flödet f(x) är i referensramen: punkt x i referensen motsvarar x + f(x) i den
globalt justerade bilden, och därmed H·(x + f(x)) i provet.
"""
from __future__ import annotations

from dataclasses import dataclass

import cv2
import numpy as np


@dataclass
class Registration:
    H: np.ndarray                    # 3x3, ref px -> prov px
    flow: np.ndarray | None          # HxWx2 i ref-px, eller None
    confidence: np.ndarray           # HxW 0..1
    rms_px: float                    # rest efter global anpassning (tile-skift)
    n_tiles: int


def to_gray_f32(rgb: np.ndarray) -> np.ndarray:
    g = cv2.cvtColor(rgb, cv2.COLOR_RGB2GRAY) if rgb.ndim == 3 else rgb
    return g.astype(np.float32) / (255.0 if g.dtype == np.uint8 else 1.0)


def warp_to_ref(sample: np.ndarray, H: np.ndarray, ref_shape, interp=cv2.INTER_LINEAR, border=255):
    h, w = ref_shape[:2]
    return cv2.warpPerspective(sample, H, (w, h), flags=interp | cv2.WARP_INVERSE_MAP,
                               borderMode=cv2.BORDER_CONSTANT, borderValue=border if sample.ndim == 2 else (border,) * 3)


def _apply_H(H, pts):
    p = np.hstack([pts, np.ones((len(pts), 1))]) @ H.T
    return p[:, :2] / p[:, 2:3]


def refine_global(ref_gray: np.ndarray, sample_gray: np.ndarray, H0: np.ndarray, valid: np.ndarray,
                  tile: int = 192, iterations: int = 3, seed: int = 1) -> tuple[np.ndarray, float, int]:
    """Förfina H0 med faskorrelation per tile och RANSAC-homografi."""
    h, w = ref_gray.shape
    H = H0.copy()
    win = cv2.createHanningWindow((tile, tile), cv2.CV_32F)
    rms, used = float("nan"), 0
    cv2.setRNGSeed(seed)
    for it in range(iterations):
        warped = warp_to_ref(sample_gray, H, ref_gray.shape, border=1.0)
        src, dst, resid = [], [], []
        step = tile // 2 if it > 0 else tile
        for y0 in range(0, h - tile + 1, step):
            for x0 in range(0, w - tile + 1, step):
                if valid[y0:y0 + tile, x0:x0 + tile].mean() < 0.6:
                    continue
                rt = ref_gray[y0:y0 + tile, x0:x0 + tile]
                if rt.std() < 0.04:
                    continue
                wt = warped[y0:y0 + tile, x0:x0 + tile]
                # OBS: OpenCV 5 multiplicerar fönstret in i indata på plats – skicka kopior.
                (dx, dy), resp = cv2.phaseCorrelate(rt.copy(), wt.copy(), win)
                if resp < 0.08 or abs(dx) > tile / 4 or abs(dy) > tile / 4:
                    continue
                c = np.array([x0 + tile / 2, y0 + tile / 2])
                src.append(c)
                dst.append(c + [dx, dy])
                resid.append(np.hypot(dx, dy))
        if len(src) < 4:
            break
        src = np.array(src, np.float64)
        dst_s = _apply_H(H, np.array(dst, np.float64))
        # Planskanner: ingen perspektivdistorsion. Affin modell (skala x/y, skjuvning) räcker,
        # och en homografi anpassad till ett veckat ark ger falska perspektivtermer.
        if len(src) >= 6:
            A, inl = cv2.estimateAffine2D(src, dst_s, method=cv2.RANSAC, ransacReprojThreshold=1.5,
                                          maxIters=2000, confidence=0.999)
        else:
            A, inl = cv2.estimateAffinePartial2D(src, dst_s, method=cv2.RANSAC, ransacReprojThreshold=1.5)
        Hn = None if A is None else np.vstack([A, [0, 0, 1]])
        if Hn is None:
            break
        H = Hn
        inl = np.ones(len(src), bool) if inl is None else inl.ravel().astype(bool)
        used = int(inl.sum())
        err = np.linalg.norm(_apply_H(H, src[inl]) - dst_s[inl], axis=1)
        rms = float(np.sqrt(np.mean(err ** 2))) if len(err) else float("nan")
    return H, rms, used


def local_flow(ref_gray: np.ndarray, warped_gray: np.ndarray, valid: np.ndarray, ppm: float,
               cell_mm: float, max_shift_mm: float, scale: float = 0.5) -> tuple[np.ndarray, np.ndarray]:
    """Tätt DIS-flöde, regulariserat till ett nät med cellstorlek cell_mm.

    Regulariseringen är avsiktligt grov: lokala fel (flyttade objekt, saknad text)
    får inte "förklaras bort" av deformationsfältet.
    """
    h, w = ref_gray.shape
    sw, sh = max(8, int(w * scale)), max(8, int(h * scale))

    def local_norm(img):
        # Lokal kontrastnormalisering: optiskt flöde kräver samma ljushet i båda bilderna,
        # vilket scannerns färg/papper/belysning annars bryter.
        x = cv2.resize(img, (sw, sh), interpolation=cv2.INTER_AREA).astype(np.float32)
        x = cv2.GaussianBlur(x, (0, 0), 1.2)               # dämpa raster och brus
        mu = cv2.GaussianBlur(x, (0, 0), 6)
        sd = np.sqrt(np.maximum(cv2.GaussianBlur((x - mu) ** 2, (0, 0), 6), 1e-6))
        # stort epsilon: jämna ytor (tonplattor, papper) förblir jämna i stället för
        # att brus förstärks till full kontrast
        z = (x - mu) / (sd + 0.08)
        return np.clip(z * 60 + 128, 0, 255).astype(np.uint8)

    r8 = local_norm(ref_gray)
    w8 = local_norm(warped_gray)
    dis = cv2.DISOpticalFlow_create(cv2.DISOPTICAL_FLOW_PRESET_MEDIUM)
    flow = dis.calc(r8, w8, None)                       # i nedskalade px

    # Struktur (gradientenergi) som vikt: flöde i tomma ytor är opålitligt.
    gx = cv2.Sobel(r8.astype(np.float32), cv2.CV_32F, 1, 0, ksize=3)
    gy = cv2.Sobel(r8.astype(np.float32), cv2.CV_32F, 0, 1, ksize=3)
    energy = cv2.GaussianBlur(gx * gx + gy * gy, (0, 0), 2.0)
    vs = cv2.resize(valid.astype(np.uint8), (sw, sh), interpolation=cv2.INTER_NEAREST).astype(bool)

    cell = max(4, int(round(cell_mm * ppm * scale)))
    ny, nx = int(np.ceil(sh / cell)), int(np.ceil(sw / cell))
    grid = np.zeros((ny, nx, 2), np.float32)
    conf = np.zeros((ny, nx), np.float32)
    e_ref = np.percentile(energy[vs], 75) if vs.any() else 1.0
    for j in range(ny):
        for i in range(nx):
            ys, xs = slice(j * cell, min(sh, (j + 1) * cell)), slice(i * cell, min(sw, (i + 1) * cell))
            m = vs[ys, xs] & (energy[ys, xs] > 0.15 * e_ref)
            if m.sum() < 0.08 * m.size:
                continue
            f = flow[ys, xs][m]
            grid[j, i] = np.median(f, axis=0)
            conf[j, i] = min(1.0, m.mean() * 2)
    # Fyll i celler utan struktur (inpainting), jämna sedan ut över grannceller.
    holes = (conf <= 0).astype(np.uint8)
    if holes.all():
        grid[:] = 0
    elif holes.any():
        for c in range(2):
            grid[..., c] = cv2.inpaint(grid[..., c].astype(np.float32), holes, 2, cv2.INPAINT_TELEA)
    # Robust mot enstaka avvikande celler (t.ex. ett flyttat objekt): median 3x3, sedan Gauss.
    if ny >= 3 and nx >= 3:
        for c in range(2):
            grid[..., c] = cv2.medianBlur(grid[..., c].astype(np.float32), 3)
    smooth = cv2.GaussianBlur(grid, (0, 0), 0.8).astype(np.float32)
    lim = max_shift_mm * ppm * scale
    mag = np.linalg.norm(smooth, axis=2, keepdims=True)
    smooth = np.where(mag > lim, smooth * (lim / np.maximum(mag, 1e-6)), smooth)

    full = cv2.resize(smooth, (w, h), interpolation=cv2.INTER_CUBIC) / scale
    # Säkerhet: i prototypen 1 överallt. Tomma ytor kan inte felregistreras på ett sätt som
    # syns, och avvikelser mellan uppmätt och utjämnat flöde kan lika gärna vara riktiga fel.
    conf_full = np.ones((h, w), np.float32)
    return full.astype(np.float32), conf_full


def build_maps(H: np.ndarray, flow: np.ndarray | None, shape) -> tuple[np.ndarray, np.ndarray]:
    h, w = shape[:2]
    xs, ys = np.meshgrid(np.arange(w, dtype=np.float32), np.arange(h, dtype=np.float32))
    if flow is not None:
        xs = xs + flow[..., 0]
        ys = ys + flow[..., 1]
    den = H[2, 0] * xs + H[2, 1] * ys + H[2, 2]
    mx = (H[0, 0] * xs + H[0, 1] * ys + H[0, 2]) / den
    my = (H[1, 0] * xs + H[1, 1] * ys + H[1, 2]) / den
    return mx.astype(np.float32), my.astype(np.float32)


def resample(sample: np.ndarray, H: np.ndarray, flow: np.ndarray | None, shape, interp=cv2.INTER_LANCZOS4) -> np.ndarray:
    """En enda omsampling från originalscanningen via sammansatt transform."""
    mx, my = build_maps(H, flow, shape)
    return cv2.remap(sample, mx, my, interp, borderMode=cv2.BORDER_CONSTANT, borderValue=(255, 255, 255))


def prepare_gray(rgb: np.ndarray, ppm: float) -> np.ndarray:
    """Gråskala, lätt utjämnad så att raster och brus inte stör korrelationen."""
    return cv2.GaussianBlur(to_gray_f32(rgb), (0, 0), max(0.8, 0.08 * ppm))


def register(ref_gs: np.ndarray, smp_gs: np.ndarray, H0: np.ndarray, valid: np.ndarray, ppm: float,
             tol: dict, seed: int = 1, local: bool = True) -> Registration:
    """ref_gs/smp_gs från prepare_gray(). ppm avser referensens arbetsupplösning."""
    tile = int(min(256, max(96, 2 ** round(np.log2(8 * ppm)))))
    H, rms, n = refine_global(ref_gs, smp_gs, H0, valid, tile=tile, seed=seed)
    flow, conf = None, np.ones(ref_gs.shape, np.float32)
    if local:
        warped = warp_to_ref(smp_gs, H, ref_gs.shape, border=1.0)
        flow, conf = local_flow(ref_gs, warped, valid, ppm, tol["local_cell_mm"], tol["max_local_shift_mm"])
        # Verifiering: behåll deformationen bara där den faktiskt förbättrar överensstämmelsen.
        flow = _verify_flow(ref_gs, smp_gs, H, flow, valid, cell_px=tol["local_cell_mm"] * ppm)
    return Registration(H=H, flow=flow, confidence=conf, rms_px=rms, n_tiles=n)


def _grad_err(ref_gs, smp_gs, H, flow, sigma):
    mx, my = build_maps(H, flow, ref_gs.shape)
    w = cv2.remap(smp_gs, mx, my, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
    # gradienter är okänsliga för ljushets- och färgskillnader
    gx_r = cv2.Sobel(ref_gs, cv2.CV_32F, 1, 0, ksize=3); gy_r = cv2.Sobel(ref_gs, cv2.CV_32F, 0, 1, ksize=3)
    gx_w = cv2.Sobel(w, cv2.CV_32F, 1, 0, ksize=3); gy_w = cv2.Sobel(w, cv2.CV_32F, 0, 1, ksize=3)
    e = np.abs(gx_r - gx_w) + np.abs(gy_r - gy_w)
    return cv2.GaussianBlur(e, (0, 0), sigma)


def _verify_flow(ref_gs, smp_gs, H, flow, valid, cell_px, min_gain=0.05):
    if flow is None:
        return None
    sig = max(2.0, cell_px / 2)
    e0 = _grad_err(ref_gs, smp_gs, H, None, sig)
    e1 = _grad_err(ref_gs, smp_gs, H, flow, sig)
    keep = (e1 < (1 - min_gain) * e0).astype(np.float32)
    keep = cv2.GaussianBlur(keep, (0, 0), sig)          # mjuk övergång mellan celler
    if keep.max() < 0.2:
        return None
    return (flow * keep[..., None]).astype(np.float32)
