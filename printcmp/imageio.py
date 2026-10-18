"""Bild-IO. Indata: TIFF (8/16 bit). PNG/JPEG accepteras i prototypen för bekvämlighet."""
from __future__ import annotations

import base64
from pathlib import Path

import cv2
import numpy as np
import tifffile


def read_scan(path: str | Path, dpi_override: float | None = None) -> tuple[np.ndarray, float]:
    """Returnerar (RGB uint8, dpi). 16-bit skalas till 8-bit i prototypen."""
    path = Path(path)
    dpi = None
    if path.suffix.lower() in (".tif", ".tiff"):
        with tifffile.TiffFile(path) as tif:
            page = tif.pages[0]
            img = page.asarray()
            tags = page.tags
            if "XResolution" in tags:
                num, den = tags["XResolution"].value
                res = num / den if den else 0
                unit = tags["ResolutionUnit"].value if "ResolutionUnit" in tags else 2
                if res > 0:
                    dpi = res * 2.54 if int(unit) == 3 else res
    else:
        bgr = cv2.imread(str(path), cv2.IMREAD_UNCHANGED)
        if bgr is None:
            raise IOError(f"kan inte läsa {path}")
        img = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB) if bgr.ndim == 3 else bgr
    if img.ndim == 2:
        img = np.stack([img] * 3, axis=-1)
    if img.shape[2] == 4:
        img = img[:, :, :3]
    if img.dtype == np.uint16:
        img = (img / 257.0 + 0.5).astype(np.uint8)
    elif img.dtype != np.uint8:
        img = np.clip(img, 0, 255).astype(np.uint8)
    if dpi_override:
        dpi = dpi_override
    if not dpi:
        raise ValueError(f"{path}: upplösning saknas i filen – ange sample.dpi i JobSpec")
    return np.ascontiguousarray(img), float(dpi)


def write_tiff(path: str | Path, rgb: np.ndarray, dpi: float) -> None:
    tifffile.imwrite(str(path), rgb, photometric="rgb", resolution=(dpi, dpi), resolutionunit="INCH")


def write_png(path: str | Path, rgb: np.ndarray) -> None:
    cv2.imwrite(str(path), cv2.cvtColor(rgb, cv2.COLOR_RGB2BGR) if rgb.ndim == 3 else rgb)


def png_data_uri(rgb: np.ndarray) -> str:
    ok, buf = cv2.imencode(".png", cv2.cvtColor(rgb, cv2.COLOR_RGB2BGR) if rgb.ndim == 3 else rgb)
    if not ok:
        raise RuntimeError("PNG-kodning misslyckades")
    return "data:image/png;base64," + base64.b64encode(buf.tobytes()).decode("ascii")


def resize_to_dpi(rgb: np.ndarray, src_dpi: float, dst_dpi: float) -> np.ndarray:
    if abs(src_dpi - dst_dpi) < 1e-6:
        return rgb
    s = dst_dpi / src_dpi
    h, w = rgb.shape[:2]
    interp = cv2.INTER_AREA if s < 1 else cv2.INTER_CUBIC
    return cv2.resize(rgb, (max(1, round(w * s)), max(1, round(h * s))), interpolation=interp)
