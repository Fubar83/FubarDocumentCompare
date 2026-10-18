"""Flera instanser (avsnitt 6.9): hitta alla förekomster av referensen på arket,
och aggregera skillnader till systematiska respektive lokala.
"""
from __future__ import annotations

from dataclasses import dataclass, field

import cv2
import numpy as np


@dataclass
class Instance:
    index: int
    center: tuple            # (x, y) i prov-px
    angle_deg: float         # total rotation (grundvinkel + skevhet)
    score: float
    row: int = -1
    col: int = -1
    H0: np.ndarray = field(default=None, repr=False)   # ref px -> prov px


def _rotate_with_canvas(img: np.ndarray, angle: float, border: float):
    h, w = img.shape[:2]
    c = (w / 2.0, h / 2.0)
    M = cv2.getRotationMatrix2D(c, angle, 1.0)
    cos, sin = abs(M[0, 0]), abs(M[0, 1])
    nw, nh = int(np.ceil(h * sin + w * cos)), int(np.ceil(h * cos + w * sin))
    M[0, 2] += nw / 2.0 - c[0]
    M[1, 2] += nh / 2.0 - c[1]
    out = cv2.warpAffine(img, M, (nw, nh), flags=cv2.INTER_LINEAR, borderMode=cv2.BORDER_CONSTANT, borderValue=border)
    return out, M


def _nms_peaks(resp: np.ndarray, min_dist: int, thr: float, max_n: int):
    r = resp.copy()
    peaks = []
    while len(peaks) < max_n:
        _, mv, _, ml = cv2.minMaxLoc(r)
        if mv < thr:
            break
        peaks.append((ml[0], ml[1], float(mv)))
        x, y = ml
        cv2.circle(r, (x, y), min_dist, -2.0, thickness=-1)
    return peaks


def similarity_H(ref_shape, ref_dpi: float, sample_dpi: float, center, angle_deg: float) -> np.ndarray:
    """H0: ref px -> prov px för en rotation kring referensens mitt och placering på center."""
    h, w = ref_shape[:2]
    s = sample_dpi / ref_dpi
    M = cv2.getRotationMatrix2D((0.0, 0.0), angle_deg, s)      # 2x3 linjär del
    T1 = np.array([[1, 0, -w / 2.0], [0, 1, -h / 2.0], [0, 0, 1]])
    R = np.vstack([M, [0, 0, 1]])
    T2 = np.array([[1, 0, center[0]], [0, 1, center[1]], [0, 0, 1]])
    return T2 @ R @ T1


def locate(ref_rgb: np.ndarray, inside: np.ndarray, ref_dpi: float, sheet_rgb: np.ndarray, sheet_dpi: float,
           rotations=(0, 90, 180, 270), max_skew_deg: float = 4.0, expected_count: int | None = None,
           coarse_dpi: float = 50.0) -> tuple[list[Instance], dict]:
    sr = coarse_dpi / ref_dpi
    ss = coarse_dpi / sheet_dpi
    rg = cv2.cvtColor(ref_rgb, cv2.COLOR_RGB2GRAY).astype(np.float32) / 255
    sg = cv2.cvtColor(sheet_rgb, cv2.COLOR_RGB2GRAY).astype(np.float32) / 255
    rs = cv2.resize(rg, None, fx=sr, fy=sr, interpolation=cv2.INTER_AREA)
    ms = cv2.resize(inside.astype(np.float32), (rs.shape[1], rs.shape[0]), interpolation=cv2.INTER_AREA)
    ms = (ms > 0.99).astype(np.float32)
    shs = cv2.resize(sg, None, fx=ss, fy=ss, interpolation=cv2.INTER_AREA)
    blur = 0.7
    rs = cv2.GaussianBlur(rs, (0, 0), blur)
    shs = cv2.GaussianBlur(shs, (0, 0), blur)
    n_exp = expected_count or 1

    def score_at(angle):
        t, _ = _rotate_with_canvas(rs, angle, 1.0)
        m, _ = _rotate_with_canvas(ms, angle, 0.0)
        m = (m > 0.5).astype(np.float32)
        if t.shape[0] >= shs.shape[0] or t.shape[1] >= shs.shape[1]:
            return -1.0, None, t.shape
        resp = cv2.matchTemplate(shs, t, cv2.TM_CCOEFF_NORMED, mask=m)
        resp = np.nan_to_num(resp, nan=-1.0, posinf=-1.0, neginf=-1.0)
        md = int(0.6 * min(t.shape))
        peaks = _nms_peaks(resp, md, 0.3, n_exp)
        if not peaks:
            return -1.0, resp, t.shape
        sc = float(np.mean([p[2] for p in peaks]))
        return sc, resp, t.shape

    best = (-2.0, 0.0)
    step = 1.0
    for base in rotations:
        for a in np.arange(-max_skew_deg, max_skew_deg + 1e-6, step):
            sc, _, _ = score_at(base + a)
            if sc > best[0]:
                best = (sc, base + a)
    lo, hi = best[1] - step, best[1] + step
    for a in np.arange(lo, hi + 1e-6, 0.2):
        sc, _, _ = score_at(a)
        if sc > best[0]:
            best = (sc, a)
    angle = float(best[1])
    info = {"sheet_angle_deg": angle, "match_score": best[0]}
    # Etiketter kan vara utskjutna i olika grundvinklar (t.ex. 0/180°) på samma ark:
    # samla toppar för varje grundvinkel med arkets skevhet och välj bland alla.
    base_best = min(rotations, key=lambda r_: abs(((angle - r_ + 180) % 360) - 180))
    skew = ((angle - base_best + 180) % 360) - 180
    cands = []
    top = -1.0
    for base in rotations:
        a_ = base + skew
        _, resp, tshape = score_at(a_)
        if resp is None:
            continue
        top = max(top, float(resp.max()))
        md = int(0.6 * min(tshape))
        for (x, y, sc) in _nms_peaks(resp, md, 0.3, expected_count or 200):
            cands.append(((x + tshape[1] / 2.0) / ss, (y + tshape[0] / 2.0) / ss, a_, sc, min(tshape) / ss))
    if not cands:
        return [], info
    thr = max(0.35, 0.6 * top)
    cands = sorted((c for c in cands if c[3] >= thr), key=lambda c: -c[3])
    max_n = expected_count if expected_count else 200
    chosen = []
    for c in cands:
        if all(np.hypot(c[0] - o[0], c[1] - o[1]) > 0.6 * c[4] for o in chosen):
            chosen.append(c)
        if len(chosen) >= max_n:
            break
    instances = []
    for i, (cx, cy, a_, sc, _) in enumerate(chosen):
        H0 = similarity_H(ref_rgb.shape, ref_dpi, sheet_dpi, (cx, cy), a_)
        instances.append(Instance(index=i + 1, center=(cx, cy), angle_deg=a_, score=sc, H0=H0))
    return instances, info


def assign_grid(instances: list[Instance], angle_deg: float, row_tol_px: float):
    """Sortera instanser i rad/kolumn-ordning (i arkets avroterade koordinatsystem)."""
    if not instances:
        return instances
    skew = ((angle_deg + 45.0) % 90.0) - 45.0          # bara skevheten, inte 90°-steg
    a = np.deg2rad(skew)
    pts = np.array([i.center for i in instances])
    R = np.array([[np.cos(a), -np.sin(a)], [np.sin(a), np.cos(a)]])
    q = pts @ R.T
    ys = q[:, 1]
    order = np.argsort(ys)
    thr = row_tol_px
    row_id = np.zeros(len(pts), int)
    r = 0
    for k in range(1, len(order)):
        if ys[order[k]] - ys[order[k - 1]] > thr:
            r += 1
        row_id[order[k]] = r
    for rr in range(r + 1):
        idx = np.where(row_id == rr)[0]
        idx = idx[np.argsort(q[idx, 0])]
        for c, k in enumerate(idx):
            instances[k].row, instances[k].col = int(rr), int(c)
    instances.sort(key=lambda i: (i.row, i.col))
    for n, inst in enumerate(instances):
        inst.index = n + 1
    return instances


def aggregate(diffs: list[dict], n_instances: int, ratio: float, cluster_mm: float = 1.0) -> list[dict]:
    """Slå ihop skillnader på samma plats i flera instanser.

    diffs: per-instans-skillnader med 'instance', 'bbox_mm', 'kind'.
    Kluster som finns i >= ratio * n_instanser blir systematiska och rapporteras en gång.
    """
    if n_instances <= 1:
        for d in diffs:
            d["systematic"] = False
            d["instances"] = [d["instance"]]
        return diffs
    pool = [d for d in diffs if d["kind"] != "Uncertain"]
    rest = [d for d in diffs if d["kind"] == "Uncertain"]

    def center(d):
        x, y, w, h = d["bbox_mm"]
        return np.array([x + w / 2, y + h / 2])

    def overlap(a, b, pad):
        ax, ay, aw, ah = a["bbox_mm"]
        bx, by, bw, bh = b["bbox_mm"]
        return not (ax - pad > bx + bw or bx - pad > ax + aw or ay - pad > by + bh or by - pad > ay + ah)

    clusters: list[list[dict]] = []
    for d in sorted(pool, key=lambda d: -d.get("area_mm2", 0)):
        placed = False
        for cl in clusters:
            ref = cl[0]
            if (np.linalg.norm(center(d) - center(ref)) <= cluster_mm or overlap(d, ref, 0.2)) and \
                    all(x["instance"] != d["instance"] for x in cl):
                cl.append(d)
                placed = True
                break
        if not placed:
            clusters.append([d])

    out = []
    need = max(2, int(np.ceil(ratio * n_instances)))

    def covered_by_uncertain(box):
        """Instanser där rutan ligger i en osäker zon (t.ex. veck)."""
        res = set()
        for u in rest:
            if overlap({"bbox_mm": box}, u, 0.0):
                res.add(u["instance"])
        return res

    for cl in clusters:
        insts = sorted({d["instance"] for d in cl})
        # instanser där felet inte kunde bedömas (osäker zon) räknas med i andelen
        unc = covered_by_uncertain(cl[0]["bbox_mm"]) - set(insts)
        if len(insts) >= 2 and len(insts) + len(unc) >= need:
            insts = sorted(set(insts) | unc)
            boxes = np.array([d["bbox_mm"] for d in cl])
            x0 = np.median(boxes[:, 0]); y0 = np.median(boxes[:, 1])
            x1 = np.median(boxes[:, 0] + boxes[:, 2]); y1 = np.median(boxes[:, 1] + boxes[:, 3])
            kinds = [d["kind"] for d in cl]
            kind = max(set(kinds), key=kinds.count)
            rep = dict(max(cl, key=lambda d: d.get("confidence", 0)))
            rep.update({"kind": kind, "systematic": True, "instances": insts,
                        "bbox_mm": [float(x0), float(y0), float(x1 - x0), float(y1 - y0)],
                        "instance": 0})
            sev_order = ["Info", "Minor", "Major", "Critical"]
            rep["severity"] = max((d["severity"] for d in cl), key=sev_order.index)
            out.append(rep)
        else:
            for d in cl:
                d["systematic"] = False
                d["instances"] = [d["instance"]]
                out.append(d)
    for d in rest:
        d["systematic"] = False
        d["instances"] = [d["instance"]]
        out.append(d)
    return out
