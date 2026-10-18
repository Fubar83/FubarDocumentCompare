"""Klassificering (avsnitt 6.8). Regler i ordning, första träff gäller."""
from __future__ import annotations

import cv2
import numpy as np

from . import barcode as bc
from .diff import Blob, DiffMaps
from .objmap import ObjEntry, overlapping

SEV = ["Info", "Minor", "Major", "Critical"]


def _moved_test(src_L: np.ndarray, dst_L: np.ndarray, box, ppm: float, max_mm: float = 4.0):
    """Sök src-utsnittet runt box i dst. Returnerar (dx, dy, score, zero_score) eller None."""
    h, w = src_L.shape
    pad = int(round(0.6 * ppm))
    x0, y0, x1, y1 = box
    x0, y0, x1, y1 = max(0, x0 - pad), max(0, y0 - pad), min(w, x1 + pad), min(h, y1 + pad)
    patch = src_L[y0:y1, x0:x1]
    if patch.size < 16 or patch.std() < 6:
        return None
    m = int(round(max_mm * ppm))
    sx0, sy0, sx1, sy1 = max(0, x0 - m), max(0, y0 - m), min(w, x1 + m), min(h, y1 + m)
    search = dst_L[sy0:sy1, sx0:sx1]
    if search.shape[0] < patch.shape[0] or search.shape[1] < patch.shape[1]:
        return None
    res = cv2.matchTemplate(search.astype(np.float32), patch.astype(np.float32), cv2.TM_CCOEFF_NORMED)
    res = np.nan_to_num(res, nan=-1)
    _, best, _, loc = cv2.minMaxLoc(res)
    zx, zy = x0 - sx0, y0 - sy0
    zero = float(res[zy, zx]) if 0 <= zy < res.shape[0] and 0 <= zx < res.shape[1] else -1.0
    return (loc[0] - zx, loc[1] - zy, float(best), zero)


def _presence_kind(rL, sL, b: Blob, ppm: float) -> str:
    """Saknas/Tillagt/Ändrat utifrån innehåll mot lokal bakgrund – inte ljushetens tecken.

    Ett borttaget vitt element på färgad botten blir mörkare i provet, men är ändå "saknat".
    """
    h, w = rL.shape
    x0, y0, x1, y1 = b.bbox
    pad = max(3, int(round(1.0 * ppm)))
    X0, Y0, X1, Y1 = max(0, x0 - pad), max(0, y0 - pad), min(w, x1 + pad), min(h, y1 + pad)
    m = np.zeros((Y1 - Y0, X1 - X0), np.uint8)
    m[y0 - Y0:y1 - Y0, x0 - X0:x1 - X0] = b.mask
    k = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * pad + 1, 2 * pad + 1))
    ring = (cv2.dilate(m, k) > 0) & (m == 0)
    rr, ss = rL[Y0:Y1, X0:X1], sL[Y0:Y1, X0:X1]
    if ring.sum() < 10:
        sdl = (ss - rr)[m > 0]
        lighter = float((sdl > 0).mean()) if sdl.size else 0.5
        return "Missing" if lighter > 0.75 else ("Added" if lighter < 0.25 else "Changed")
    # bakgrunden: median av ringen i båda bilderna (där de är lika)
    bg_r, bg_s = float(np.median(rr[ring])), float(np.median(ss[ring]))
    inner = m > 0
    c_ref = float(np.mean(np.abs(rr[inner] - bg_r)))
    c_smp = float(np.mean(np.abs(ss[inner] - bg_s)))
    hi, lo = max(c_ref, c_smp), min(c_ref, c_smp)
    if hi < 1e-6 or lo > 0.45 * hi:
        return "Changed"
    return "Missing" if c_ref > c_smp else "Added"


def classify(blobs: list[Blob], maps: DiffMaps, objs: list[ObjEntry], ref_codes: list[bc.Code],
             smp_rgb_u8: np.ndarray, fold_band: np.ndarray, confidence: np.ndarray,
             ppm: float, dpi: float, tol: dict, instance: int) -> list[dict]:
    rL = maps.ref_lab[..., 0]
    sL = maps.smp_lab[..., 0]
    out: list[dict] = []
    for b in blobs:
        x0, y0, x1, y1 = b.bbox
        sub_fold = fold_band[y0:y1, x0:x1][b.mask]
        sub_conf = confidence[y0:y1, x0:x1][b.mask]
        d = {
            "instance": instance, "kind": None, "severity": "Minor",
            "bbox_px": [int(x0), int(y0), int(x1 - x0), int(y1 - y0)],
            "bbox_mm": [x0 / ppm, y0 / ppm, (x1 - x0) / ppm, (y1 - y0) / ppm],
            "area_mm2": b.area_px / (ppm * ppm),
            "objects": [], "metrics": {"delta_l": round(b.max_dl, 2), "delta_e": round(b.mean_de, 2), "shift_mm": None},
            "confidence": 0.9,
        }
        objs_hit = overlapping(objs, b.bbox, kinds=("Text", "Image", "Vector"), min_frac=0.02)
        d["objects"] = [{"id": o.id, "kind": o.kind, **({"text": o.text} if o.text else {})} for o in objs_hit[:8]]
        text_hit = any(o.kind == "Text" for o in objs_hit)

        if sub_fold.size and sub_fold.mean() > 0.3:
            d.update(kind="Uncertain", severity="Info", cause="fold", confidence=0.5)
            out.append(d)
            continue
        if sub_conf.size and sub_conf.mean() < 0.15:
            d.update(kind="Uncertain", severity="Info", cause="registration", confidence=0.4)
            out.append(d)
            continue

        code_hit = None
        for c in ref_codes:
            cx0, cy0, cx1, cy1 = c.bbox
            if not (x1 < cx0 or cx1 < x0 or y1 < cy0 or cy1 < y0):
                code_hit = c
                break
        if code_hit is not None:
            got = bc.decode_region(smp_rgb_u8, code_hit.bbox, int(2 * ppm), dpi)
            match = [g for g in got if g.data == code_hit.data]
            if match:
                g = match[0]
                # Detektorns hörnpunkter är för grova för att mäta läget; mät förskjutningen
                # med faskorrelation mellan referensens och provets kodyta.
                pad = int(2 * ppm)
                hh, ww = rL.shape
                cx0, cy0 = max(0, int(code_hit.bbox[0]) - pad), max(0, int(code_hit.bbox[1]) - pad)
                cx1, cy1 = min(ww, int(code_hit.bbox[2]) + pad), min(hh, int(code_hit.bbox[3]) + pad)
                a_ = rL[cy0:cy1, cx0:cx1].astype(np.float32).copy()
                b_ = sL[cy0:cy1, cx0:cx1].astype(np.float32).copy()
                win = cv2.createHanningWindow((a_.shape[1], a_.shape[0]), cv2.CV_32F)
                (sdx, sdy), resp = cv2.phaseCorrelate(a_, b_, win)
                shift_mm = float(np.hypot(sdx, sdy) / ppm) if resp > 0.2 else 0.0
                if shift_mm >= 0.25:
                    # rätt innehåll men fel plats: ett layoutfel
                    x0u = min(code_hit.bbox[0], g.bbox[0]); y0u = min(code_hit.bbox[1], g.bbox[1])
                    x1u = max(code_hit.bbox[2], g.bbox[2]); y1u = max(code_hit.bbox[3], g.bbox[3])
                    d.update(kind="Moved", severity="Major",
                             bbox_px=[int(x0u), int(y0u), int(x1u - x0u), int(y1u - y0u)],
                             bbox_mm=[x0u / ppm, y0u / ppm, (x1u - x0u) / ppm, (y1u - y0u) / ppm],
                             barcode={"expected": code_hit.data, "read": g.data})
                    d["metrics"]["shift_mm"] = round(shift_mm, 3)
                    if not any(o.get("kind") == "Moved" and o.get("barcode") for o in out):
                        out.append(d)
                # Annars: formavvikelser inom en korrekt läst kod är en kvalitetsfråga
                # (gradering enligt ISO 15416/15415), inte ett innehållsfel.
                continue
            else:
                read = got[0].data if got else None
                d.update(kind="Barcode", severity="Critical",
                         barcode={"expected": code_hit.data, "read": read, "symbology": code_hit.symbology})
                cx0, cy0, cx1, cy1 = code_hit.bbox
                d["bbox_px"] = [int(cx0), int(cy0), int(cx1 - cx0), int(cy1 - cy0)]
                d["bbox_mm"] = [cx0 / ppm, cy0 / ppm, (cx1 - cx0) / ppm, (cy1 - cy0) / ppm]
            out.append(d)
            continue

        if b.kind_hint == "color":
            d["kind"] = "Color"
            d["severity"] = "Major" if b.mean_de > 2 * tol["delta_e"] else "Minor"
            out.append(d)
            continue

        kind = _presence_kind(rL, sL, b, ppm)
        if kind == "Changed" and not text_hit:
            kind = "Shape"
        # Flyttat? Sök referensinnehållet i provet (eller tvärtom för tillagt innehåll).
        mt = _moved_test(rL, sL, b.bbox, ppm) if kind != "Added" else _moved_test(sL, rL, b.bbox, ppm)
        if mt is not None:
            dx, dy, best, zero = mt
            if kind == "Added":
                dx, dy = -dx, -dy
            shift_mm = float(np.hypot(dx, dy) / ppm)
            if best > 0.8 and best - zero > 0.1 and shift_mm >= 0.12:
                kind = "Moved"
                d["metrics"]["shift_mm"] = round(shift_mm, 3)
                d["shift_px"] = [int(dx), int(dy)]
        d["kind"] = kind
        if text_hit or d["area_mm2"] > 4.0:
            d["severity"] = "Major"
        out.append(d)

    out = _merge_moved(out, ppm)
    # En felaktig streckkod rapporteras en gång: ta bort övriga skillnader inom kodens yta
    # (staplar och klarskrift under koden).
    bad = [d for d in out if d["kind"] == "Barcode"]
    if bad:
        keep = []
        for d in out:
            if d["kind"] != "Barcode" and d.get("bbox_px"):
                x, y, w_, h_ = d["bbox_px"]
                cx, cy = x + w_ / 2, y + h_ / 2
                pad = 1.5 * ppm
                if any(bx - pad <= cx <= bx + bw + pad and by - pad <= cy <= by + bh + 4 * ppm
                       for bx, by, bw, bh in (b_["bbox_px"] for b_ in bad)):
                    continue
            keep.append(d)
        out = keep
    return out


def _merge_moved(diffs: list[dict], ppm: float) -> list[dict]:
    """Slå ihop Missing/Added/Moved-par som beskriver samma förflyttning."""
    used = set()
    merged = []
    for i, a in enumerate(diffs):
        if i in used or a["kind"] not in ("Moved", "Missing", "Added", "Changed", "Shape"):
            continue
        for j, b in enumerate(diffs):
            if j <= i or j in used or b["kind"] not in ("Moved", "Missing", "Added", "Changed", "Shape"):
                continue
            if "shift_px" not in a and "shift_px" not in b:
                continue
            s = a.get("shift_px") or [-v for v in b["shift_px"]]
            ax, ay, aw, ah = a["bbox_px"]
            bx, by, bw, bh = b["bbox_px"]
            # a flyttat med s ska hamna på b
            tx, ty = ax + s[0], ay + s[1]
            ix = max(0, min(tx + aw, bx + bw) - max(tx, bx))
            iy = max(0, min(ty + ah, by + bh) - max(ty, by))
            if ix * iy > 0.25 * min(aw * ah, bw * bh):
                x0, y0 = min(ax, bx), min(ay, by)
                x1, y1 = max(ax + aw, bx + bw), max(ay + ah, by + bh)
                a = dict(a)
                a["kind"] = "Moved"
                a["bbox_px"] = [x0, y0, x1 - x0, y1 - y0]
                a["bbox_mm"] = [x0 / ppm, y0 / ppm, (x1 - x0) / ppm, (y1 - y0) / ppm]
                a["area_mm2"] = a["area_mm2"] + b["area_mm2"]
                a["metrics"]["shift_mm"] = round(float(np.hypot(*s) / ppm), 3)
                a["shift_px"] = s
                a["severity"] = SEV[max(SEV.index(a["severity"]), SEV.index(b["severity"]))]
                a["objects"] = a["objects"] + [o for o in b["objects"] if o not in a["objects"]]
                used.add(j)
        used.add(i)
        merged.append(a)
    merged.extend(d for k, d in enumerate(diffs) if k not in used)
    return merged
