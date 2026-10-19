"""Klassificering (avsnitt 6.8). Regler i ordning, första träff gäller."""
from __future__ import annotations

import cv2
import numpy as np

from . import barcode as bc
from .diff import Blob, DiffMaps
from .objmap import ObjEntry, overlapping
from . import structure

SEV = ["Info", "Minor", "Major", "Critical"]

# Lägsta allvarlighet per objektkategori (innehåll som påverkar läsbarhet/regelefterlevnad)
MIN_SEV = {"Text": "Major", "OutlinedText": "Major", "TableCell": "Major", "TableRule": "Major",
           "Barcode": "Critical", "Braille": "Critical"}
CAT_ORDER = ("Text", "OutlinedText", "Barcode", "Image", "Rule", "Vector")


def _raise(sev: str, floor: str) -> str:
    return SEV[max(SEV.index(sev), SEV.index(floor))]


def _tone_offset(rL, sL, maps, b: Blob, objs_hit, ppm: float):
    """Förklaras skillnaden av en enhetlig tonförskjutning i hela objektet?

    Typiskt vid okalibrerad scanner och unika färger: samma form, annan ljushet.
    Returnerar {"offset", "delta_e"} eller None.
    """
    cands = [o for o in objs_hit if o.kind in ("Text", "OutlinedText", "Vector")]
    if not cands:
        return None
    o = max(cands, key=lambda o: (o.bbox[2] - o.bbox[0]) * (o.bbox[3] - o.bbox[1]))
    h, w = rL.shape
    pad = int(0.3 * ppm)
    X0, Y0 = max(0, int(o.bbox[0]) - pad), max(0, int(o.bbox[1]) - pad)
    X1, Y1 = min(w, int(o.bbox[2]) + pad), min(h, int(o.bbox[3]) + pad)
    x0, y0, x1, y1 = b.bbox
    blob_ref = rL[y0:y1, x0:x1][b.mask]
    blob_dif = (sL[y0:y1, x0:x1] - rL[y0:y1, x0:x1])[b.mask]
    if blob_ref.size < 5:
        return None
    tone_ref = float(np.median(blob_ref))
    R = rL[Y0:Y1, X0:X1]
    same = np.abs(R - tone_ref) < 6.0
    # bara objektets "inre" (bort från kanter) räknas, så registreringsfel inte spelar in
    same &= cv2.erode(same.astype(np.uint8), np.ones((3, 3), np.uint8)).astype(bool)
    if same.sum() < max(50, 4 * blob_ref.size):
        return None
    D = (sL[Y0:Y1, X0:X1] - R)[same]
    off = float(np.median(D))
    if abs(off) < 8.0:
        return None
    spread = float(np.median(np.abs(D - off)))
    if spread > 0.35 * abs(off) or abs(float(np.median(blob_dif)) - off) > 0.35 * abs(off) + 3.0:
        return None
    de = float(np.median(maps.delta_e[Y0:Y1, X0:X1][same]))
    return {"offset": off, "delta_e": de}


def _near_text_line(objs, box) -> bool:
    """Ligger rutan på samma rad som befintlig text, nära i sidled?"""
    cy = (box[1] + box[3]) / 2
    for o in objs:
        if o.kind not in ("Text", "OutlinedText"):
            continue
        h = o.bbox[3] - o.bbox[1]
        if not (o.bbox[1] - 0.2 * h <= cy <= o.bbox[3] + 0.2 * h):
            continue
        gap = max(box[0] - o.bbox[2], o.bbox[0] - box[2], 0)
        if gap <= 2.0 * h:
            return True
    return False


def category_of(objs_hit, table_info) -> str:
    if table_info:
        return "TableRule" if table_info["element"] == "rule" else "TableCell"
    kinds = {o.kind for o in objs_hit}
    for k in CAT_ORDER:
        if k in kinds:
            return k
    return "Background"


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
    # Fanns samma innehåll redan på målplatsen i källbilden (t.ex. ett annat "g" i ordet)?
    # Då är det ingen flytt, bara en likadan form i närheten.
    dx, dy = loc[0] - zx, loc[1] - zy
    tx0, ty0 = x0 + dx, y0 + dy
    own = src_L[ty0:ty0 + patch.shape[0], tx0:tx0 + patch.shape[1]]
    if own.shape == patch.shape and own.std() > 1e-3:
        same = float(cv2.matchTemplate(own.astype(np.float32), patch.astype(np.float32), cv2.TM_CCOEFF_NORMED)[0, 0])
        if same > best - 0.1:
            return None
    return (dx, dy, float(best), zero)


def _inside_word(objs_hit, box, ppm: float) -> bool:
    """Blobben ligger inne i ett ord och täcker bara en liten del av det."""
    x0, y0, x1, y1 = box
    pad = 0.15 * ppm
    for o in objs_hit:
        if o.kind not in ("Text", "OutlinedText"):
            continue
        ox0, oy0, ox1, oy1 = o.bbox
        if x0 >= ox0 - pad and y0 >= oy0 - pad and x1 <= ox1 + pad and y1 <= oy1 + pad:
            if (x1 - x0) * (y1 - y0) < 0.4 * (ox1 - ox0) * (oy1 - oy0):
                return True
    return False


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
             ppm: float, dpi: float, tol: dict, instance: int, tables=()) -> list[dict]:
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
        objs_hit = overlapping(objs, b.bbox, kinds=("Text", "OutlinedText", "Image", "Vector", "Rule", "Barcode"),
                               min_frac=0.02)
        d["objects"] = [{"id": o.id, "kind": o.kind, **({"text": o.text} if o.text else {})} for o in objs_hit[:8]]
        tinfo = structure.locate(tables, b.bbox, ppm) if tables else None
        if tinfo is not None:
            d["table"] = tinfo
        d["category"] = category_of(objs_hit, tinfo)
        if d["category"] == "Background" and _near_text_line(objs, b.bbox):
            d["category"] = "Text"          # t.ex. tillagd text i förlängningen av en rad
        text_hit = d["category"] in ("Text", "OutlinedText", "TableCell")

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

        tone = _tone_offset(rL, sL, maps, b, objs_hit, ppm)
        if tone is not None:
            # hela objektet har samma ljushetsförskjutning: tonavvikelse (färg), inte form
            if tone["delta_e"] < tol["delta_e"]:
                continue
            d["kind"] = "Color"
            d["severity"] = "Major" if tone["delta_e"] > 2 * tol["delta_e"] else "Minor"
            d["metrics"]["delta_e"] = round(tone["delta_e"], 2)
            d["tone_offset_L"] = round(tone["offset"], 1)
            out.append(d)
            continue
        kind = _presence_kind(rL, sL, b, ppm)
        if kind in ("Added", "Missing") and text_hit and _inside_word(objs_hit, b.bbox, ppm):
            # ett streck mer eller mindre inne i ett befintligt ord (6 -> 8, g -> q) är
            # en teckenändring, inte tillagt/saknat innehåll
            kind = "Changed"
        if kind == "Changed" and not text_hit:
            kind = "Shape"
        # Flyttat? Sök referensinnehållet i provet (eller tvärtom för tillagt innehåll).
        # Linjer hoppas över: en saknad linje "hittas" annars i grannlinjen.
        if d["category"] in ("TableRule", "Rule"):
            mt = None
        else:
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

    for d in out:
        if d.get("category") in MIN_SEV and d["kind"] not in ("Uncertain",):
            d["severity"] = _raise(d["severity"], MIN_SEV[d["category"]])
    out = _merge_moved(out, ppm)
    out = _merge_table_rows(out, ppm)
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


def _merge_table_rows(diffs: list[dict], ppm: float) -> list[dict]:
    """Flera "Flyttat" i samma tabellrad med samma förskjutning = en förskjuten rad."""
    groups: dict = {}
    rest = []
    for d in diffs:
        t = d.get("table")
        if d["kind"] == "Moved" and t and t.get("element") == "cell" and d.get("shift_px"):
            groups.setdefault((t["table"], t["row"]), []).append(d)
        else:
            rest.append(d)
    out = list(rest)
    for (tid, row), ds in groups.items():
        if len(ds) == 1:
            out.append(ds[0])
            continue
        sh = np.array([d["shift_px"] for d in ds], float)
        if np.max(np.linalg.norm(sh - sh.mean(axis=0), axis=1)) > 0.15 * ppm:
            out.extend(ds)
            continue
        x0 = min(d["bbox_px"][0] for d in ds); y0 = min(d["bbox_px"][1] for d in ds)
        x1 = max(d["bbox_px"][0] + d["bbox_px"][2] for d in ds); y1 = max(d["bbox_px"][1] + d["bbox_px"][3] for d in ds)
        m = dict(ds[0])
        m["bbox_px"] = [x0, y0, x1 - x0, y1 - y0]
        m["bbox_mm"] = [x0 / ppm, y0 / ppm, (x1 - x0) / ppm, (y1 - y0) / ppm]
        m["area_mm2"] = sum(d["area_mm2"] for d in ds)
        m["table"] = dict(ds[0]["table"], col=None, cell_text=None, row_shift=True)
        m["objects"] = [o for d in ds for o in d["objects"]][:8]
        out.append(m)
    return out
