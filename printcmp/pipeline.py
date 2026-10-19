"""Orkestrering av ett jobb: JobSpec in, resultat (dict/JSON) och bildlager ut."""
from __future__ import annotations

import hashlib
import json
import time
from pathlib import Path

import cv2
import numpy as np

from . import __version__
from . import (barcode, braille, certainty, classify, layers, textdiff, diff, folds, imageio, instances, mask, normalize, objmap, pdfspots,
               register, structure)
from .render import get_renderer

STAGES = ["render", "locate", "register", "normalize", "diff", "classify", "aggregate", "report"]


class Timer:
    def __init__(self):
        self.t = {}

    def add(self, k, dt):
        self.t[k] = self.t.get(k, 0.0) + dt


def _sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return "sha256:" + h.hexdigest()


def _canonical(obj) -> str:
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def run(spec: dict, progress=None) -> dict:
    def prog(stage, frac):
        if progress:
            progress(stage, frac)

    T = Timer()
    tol = spec["tolerances"]
    dpi = float(spec["work_dpi"])
    ppm = dpi / 25.4
    seed = int(spec.get("seed", 1))
    out_dir = Path(spec["outputs"]["dir"])
    out_dir.mkdir(parents=True, exist_ok=True)
    rules = dict(pdfspots.DEFAULT_RULES)
    rules.update(spec.get("spot_rules") or {})
    lspec = spec.get("layers") or {}
    rules.update(layers.spot_rules(exclude_spots=lspec.get("exclude_spots")))

    # ---------- referens ----------
    t0 = time.perf_counter()
    prog("render", 0.0)
    ref_bytes = Path(spec["reference"]["path"]).read_bytes()
    page = spec["reference"]["page"] - 1
    layer_report = {"reference": _layer_json(layers.list_layers(ref_bytes, page)), "rules": lspec}
    ref_bytes, layer_report["reference"]["compared"] = layers.filter_pdf(ref_bytes, page, lspec.get("include"),
                                                                         lspec.get("exclude"))
    spots = pdfspots.analyze(ref_bytes, page, rules)
    renderer = get_renderer("pdfium")
    ref = renderer.render(spots.sanitized_pdf, page, dpi, spec["reference"]["box"])
    ref_codes = barcode.detect(ref.rgb, dpi)
    objs = objmap.build(spots.sanitized_pdf, page, ref.geom, [c.bbox for c in ref_codes])
    tables = structure.detect(objs, ppm)
    blines = braille.from_pdf(spots, ref_bytes, page, ref.geom)
    bmask = braille.mask(ref.rgb.shape, blines, ppm) if blines else None
    masks = mask.build(ref.rgb.shape, ref.geom, spots, spec["masks"], erode_mm=tol["dieline_margin_mm"],
                       braille_mask=bmask)
    text_mask = np.zeros(ref.rgb.shape[:2], bool)
    pad = max(1, int(round(0.2 * ppm)))
    for o in objs:
        # Bara liten text (brödtext, tabellceller): rubriker har signal nog och deras
        # rasterkanter mot färgad botten skulle ge falsklarm med den lägre tröskeln.
        if o.kind == "Text" and (o.bbox[3] - o.bbox[1]) <= tol["text_small_mm"] * ppm:
            x0, y0, x1, y1 = (int(round(v)) for v in o.bbox)
            text_mask[max(0, y0 - pad):y1 + pad, max(0, x0 - pad):x1 + pad] = True

    # ---------- prov ----------
    kind = spec["sample"]["kind"]
    if kind == "pdf":
        sb = Path(spec["sample"]["path"]).read_bytes()
        layer_report["sample"] = _layer_json(layers.list_layers(sb, spec["sample"]["page"] - 1))
        sb, layer_report["sample"]["compared"] = layers.filter_pdf(sb, spec["sample"]["page"] - 1,
                                                                   lspec.get("include"), lspec.get("exclude"))
        sspots = pdfspots.analyze(sb, spec["sample"]["page"] - 1, rules)
        srend = renderer.render(sspots.sanitized_pdf, spec["sample"]["page"] - 1, dpi, spec["reference"]["box"])
        sample_rgb = srend.rgb
        sample_dpi = dpi
        sample_bdots, _ = braille.dots_from_spots(sspots, srend.geom)
        if not sample_bdots:
            sample_bdots, _ = braille.dots_from_text(sb, spec["sample"]["page"] - 1, srend.geom)
    else:
        sample_rgb, sample_dpi = imageio.read_scan(spec["sample"]["path"], spec["sample"].get("dpi"))
    T.add("render", time.perf_counter() - t0)

    # ---------- lokalisering ----------
    t0 = time.perf_counter()
    prog("locate", 0.0)
    ispec = spec["instances"]
    layout = ispec.get("expected_layout") or {}
    expected = ispec.get("expected_count") or (layout.get("rows", 0) * layout.get("cols", 0)) or \
        (1 if ispec["mode"] == "single" else None)
    if kind == "pdf" and ispec["mode"] == "single":
        H0 = instances.similarity_H(ref.rgb.shape, dpi, sample_dpi,
                                    (sample_rgb.shape[1] / 2, sample_rgb.shape[0] / 2), 0.0)
        insts = [instances.Instance(1, (sample_rgb.shape[1] / 2, sample_rgb.shape[0] / 2), 0.0, 1.0, 0, 0, H0)]
        loc_info = {"sheet_angle_deg": 0.0, "match_score": 1.0}
    else:
        insts, loc_info = instances.locate(ref.rgb, masks.inside, dpi, sample_rgb, sample_dpi,
                                           rotations=ispec["rotations"], max_skew_deg=ispec["max_skew_deg"],
                                           expected_count=expected)
        row_tol = 0.4 * min(ref.rgb.shape[:2]) * sample_dpi / dpi
        insts = instances.assign_grid(insts, loc_info["sheet_angle_deg"], row_tol)
    T.add("locate", time.perf_counter() - t0)

    # ---------- per instans ----------
    ref_f = normalize.to_float(ref.rgb)
    ref_gs = register.prepare_gray(ref.rgb, ppm)
    smp_gs = register.prepare_gray(sample_rgb, sample_dpi / 25.4)
    sigma = 0.0
    period = None
    extra_blur = 0.0
    is_scan = kind != "pdf"
    all_diffs: list[dict] = []
    inst_out = []
    aligned_L = []
    bands = []
    layers_dir = out_dir / "layers"
    if spec["outputs"]["layers"]:
        layers_dir.mkdir(exist_ok=True)
    layer_scale = min(1.0, spec["outputs"]["layer_dpi"] / dpi)
    ref_d = ref_f

    states = []
    for k, inst in enumerate(insts):
        prog("register", k / max(1, len(insts)))
        t0 = time.perf_counter()
        reg = register.register(ref_gs, smp_gs, inst.H0, masks.valid, ppm, tol, seed=seed,
                                local=tol["local_registration"] and is_scan)
        aligned = register.resample(sample_rgb, reg.H, reg.flow, ref.rgb.shape)
        T.add("register", time.perf_counter() - t0)

        t0 = time.perf_counter()
        smp_f = normalize.to_float(aligned)
        if k == 0 and tol["descreen"] and is_scan:
            period = normalize.estimate_screen_period_px(
                cv2.cvtColor(aligned, cv2.COLOR_RGB2GRAY).astype(np.float32) / 255,
                cv2.cvtColor(ref.rgb, cv2.COLOR_RGB2GRAY).astype(np.float32) / 255, masks.valid)
            # utan tonplattor går rastret inte att mäta: anta typiskt raster 70 linjer/cm
            sigma = float(np.clip(0.55 * (period if period else dpi / (70 * 2.54)), 1.0, 3.0))
            ref_d = normalize.descreen(ref_f, sigma)
        smp_d = normalize.descreen(smp_f, sigma) if sigma > 0 else smp_f
        if k == 0 and is_scan and tol["normalize"]:
            # matcha skärpan: provet är oskarpare (scannerns MTF) – sudda referensen lika mycket
            extra_blur = normalize.estimate_extra_blur(ref_d, smp_d, masks.valid)
            if extra_blur > 0:
                ref_d = normalize.descreen(ref_d, extra_blur)
        color_M = None
        if tol["normalize"] and is_scan:
            # färg först (global), sedan belysning (lågfrekvent rest), sedan färg igen
            color_M = normalize.fit_color(smp_d, ref_d, masks.valid)
            smp_d = normalize.apply_color(smp_d, color_M)
            if tol["illumination_flatten"]:
                smp_d, _ = normalize.flatten_illumination(smp_d, ref_d, masks.valid, ppm)
                color_M = normalize.fit_color(smp_d, ref_d, masks.valid)
                smp_d = normalize.apply_color(smp_d, color_M)
        T.add("normalize", time.perf_counter() - t0)

        t0 = time.perf_counter()
        maps = diff.compute(ref_d, smp_d, masks.valid, masks.varnish, ppm, tol, text_mask)
        if tol["fold_detection"] and is_scan:
            fl, _ = folds.detect(maps.ref_lab[..., 0], maps.smp_lab[..., 0], masks.valid, ppm, tol["fold_band_mm"])
        else:
            fl = []
        T.add("diff", time.perf_counter() - t0)
        states.append({"inst": inst, "reg": reg, "aligned": aligned, "maps": maps, "fl": fl, "color_M": color_M})

    # ---------- veck på arknivå ----------
    # Ett veck är en rak linje över hela arket. Ett veck som hittas säkert i en instans
    # förs över till alla instanser som linjen korsar (svaga veck missas annars där de
    # korsar streckkoder eller tryckta ytor).
    # Punkter längs varje veckssegment (i arkets koordinater) klustras och anpassas med
    # PCA till en linje per veck – det ger rätt vinkel även långt från där vecket syntes.
    _drop_systematic_fold_candidates(states, ref.rgb.shape[:2], ppm, sample_dpi / 25.4)
    segs = []
    for st in states:
        H = st["reg"].H
        for f in st["fl"]:
            t = np.linspace(0, 1, 25)[:, None]
            pts = np.array(f.p0) * (1 - t) + np.array(f.p1) * t
            q = np.hstack([pts, np.ones((len(pts), 1))]) @ H.T
            segs.append((q[:, :2] / q[:, 2:3], f.band_px))
    clusters = []      # [points, band]
    tol_px = 3.0 * sample_dpi / 25.4
    for pts, bw in segs:
        d = pts[-1] - pts[0]
        d = d / max(np.linalg.norm(d), 1e-9)
        placed = False
        for cl in clusters:
            c_pts = cl[0]
            mu = c_pts.mean(axis=0)
            _, _, vt = np.linalg.svd(c_pts - mu, full_matrices=False)
            e = vt[0]
            n = np.array([-e[1], e[0]])
            if abs(float(np.dot(d, e))) > 0.99 and np.abs((pts - mu) @ n).max() < tol_px:
                cl[0] = np.vstack([c_pts, pts])
                cl[1] = max(cl[1], bw)
                placed = True
                break
        if not placed:
            clusters.append([pts, bw])
    merged = []
    for c_pts, bw in clusters:
        mu = c_pts.mean(axis=0)
        _, _, vt = np.linalg.svd(c_pts - mu, full_matrices=False)
        e = vt[0]
        merged.append((mu - 1e5 * e, mu + 1e5 * e, bw))
    h_ref, w_ref = ref.rgb.shape[:2]
    if merged:
        # Pröva varje veckhypotes mot alla etiketter den korsar. Ett verkligt veck syns i
        # (nästan) alla; en enstaka kant i en etikett (ett tryck- eller originalfel) gör inte det.
        rms = [folds.residual_map(st["maps"].ref_lab[..., 0], st["maps"].smp_lab[..., 0], masks.valid, ppm)
               for st in states]
        keep = []
        for p0, p1, bw in merged:
            n_cross = n_sup = 0
            for st, rm in zip(states, rms):
                q = np.array([[*p0, 1.0], [*p1, 1.0]]) @ np.linalg.inv(st["reg"].H).T
                q = q[:, :2] / q[:, 2:3]
                frac, n = folds.support(rm, folds.Fold(tuple(q[0]), tuple(q[1]), bw))
                if n * rm["ds"] < 8 * ppm:          # korsar för lite ljust papper för att avgöra
                    continue
                n_cross += 1
                n_sup += frac >= 0.4
            if n_cross <= 1 or n_sup >= max(2, int(np.ceil(0.5 * n_cross))):
                keep.append((p0, p1, bw))
        merged = keep
    for st in states:
        Hinv = np.linalg.inv(st["reg"].H)
        fl_all = []
        for p0, p1, bw in merged:
            # korta ner till en lagom lång bit kring etiketten innan transformen
            mid = (p0 + p1) / 2
            e = (p1 - p0) / np.linalg.norm(p1 - p0)
            c_lab = st["reg"].H @ np.array([w_ref / 2, h_ref / 2, 1.0])
            c_lab = c_lab[:2] / c_lab[2]
            foot = mid + np.dot(c_lab - mid, e) * e
            a_, b_ = foot - e * 2 * (w_ref + h_ref), foot + e * 2 * (w_ref + h_ref)
            q = np.array([[a_[0], a_[1], 1.0], [b_[0], b_[1], 1.0]]) @ Hinv.T
            q = q[:, :2] / q[:, 2:3]
            f = folds.Fold(tuple(q[0]), tuple(q[1]), bw)
            f = folds.refine_near(st["maps"].ref_lab[..., 0], st["maps"].smp_lab[..., 0], masks.valid, ppm, f)
            if any(folds._point_line_dist(np.array(f.p0) / 2 + np.array(f.p1) / 2, np.array(g.p0), np.array(g.p1))
                   < 2 * bw and folds._angle_diff(f.p0, f.p1, g.p0, g.p1) < 5 for g in fl_all):
                continue
            zone = np.zeros((h_ref, w_ref), np.uint8)
            folds._draw_extended_band(zone, f, w_ref, h_ref)
            if (zone.astype(bool) & masks.valid).any():
                # bandets bredd efter hur långt vecket faktiskt påverkar bilden
                hw = folds.estimate_halfwidth(st["maps"].d_struct, st["maps"].smp_lab[..., 0] - st["maps"].ref_lab[..., 0],
                                              masks.valid, ppm, f, 0.6 * tol["struct_dl"])
                f = folds.Fold(f.p0, f.p1, max(f.band_px, hw + 0.5 * ppm))
                fl_all.append(f)
        band = np.zeros((h_ref, w_ref), np.uint8)
        for f in fl_all:
            folds._draw_extended_band(band, f, w_ref, h_ref)
        st["fl"], st["band"] = fl_all, band.astype(bool)

    for st in states:
        inst, reg, aligned, maps, fl, band, color_M = (st[k] for k in ("inst", "reg", "aligned", "maps", "fl", "band", "color_M"))
        t0 = time.perf_counter()
        # Inom veckband går det inte att skilja veck från fel: utvärdera inte där,
        # rapportera i stället en osäker zon per veck.
        maps.struct_mask &= ~band
        maps.color_mask &= ~band
        blobs = diff.blobs(maps, ppm, tol)
        T.add("diff", time.perf_counter() - t0)

        t0 = time.perf_counter()
        dl = classify.classify(blobs, maps, objs, ref_codes, aligned, band, reg.confidence, ppm, dpi, tol, inst.index,
                               tables=tables)
        b_reads, b_stats = [], []
        if blines:
            if is_scan:
                bd, b_reads, b_stats = braille.check_scan(maps.smp_lab[..., 0] - maps.ref_lab[..., 0], blines, ppm,
                                                          inst.index, band)
            else:
                Hinv = np.linalg.inv(reg.H)
                pts = np.array([[x, y, 1.0] for x, y in sample_bdots]).reshape(-1, 3) @ Hinv.T
                pts = pts[:, :2] / pts[:, 2:3] if len(pts) else pts[:, :2]
                bd, b_reads = braille.compare_dots(blines, [tuple(p) for p in pts], ppm, inst.index)
            dl.extend(bd)
        for f in fl:
            zone = np.zeros(band.shape, np.uint8)
            folds._draw_extended_band(zone, f, band.shape[1], band.shape[0])
            ys, xs = np.nonzero(zone.astype(bool) & masks.valid)
            if len(xs):
                x0, y0, x1, y1 = int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1
                dl.append({"instance": inst.index, "kind": "Uncertain", "severity": "Info", "cause": "fold",
                           "bbox_px": [x0, y0, x1 - x0, y1 - y0],
                           "bbox_mm": [x0 / ppm, y0 / ppm, (x1 - x0) / ppm, (y1 - y0) / ppm],
                           "area_mm2": float(len(xs)) / ppm ** 2, "objects": [],
                           "metrics": {"delta_l": None, "delta_e": None, "shift_mm": None}, "confidence": 0.5,
                           "line_mm": [[f.p0[0] / ppm, f.p0[1] / ppm], [f.p1[0] / ppm, f.p1[1] / ppm]]})
        # Explicit streckkodskontroll även om diffen inte flaggade koden
        for c in ref_codes:
            covered = any(d["kind"] in ("Barcode", "Shape") and d.get("barcode", {}).get("expected") == c.data
                          and d["instance"] == inst.index for d in dl)
            if covered:
                continue
            got = barcode.decode_region(aligned, c.bbox, int(2 * ppm), dpi)
            if not any(g.data == c.data for g in got):
                x0, y0, x1, y1 = c.bbox
                dl.append({"instance": inst.index, "kind": "Barcode", "severity": "Critical",
                           "bbox_px": [int(x0), int(y0), int(x1 - x0), int(y1 - y0)],
                           "bbox_mm": [x0 / ppm, y0 / ppm, (x1 - x0) / ppm, (y1 - y0) / ppm],
                           "area_mm2": (x1 - x0) * (y1 - y0) / ppm ** 2, "objects": [],
                           "metrics": {"delta_l": None, "delta_e": None, "shift_mm": None}, "confidence": 0.8,
                           "barcode": {"expected": c.data, "read": got[0].data if got else None,
                                       "symbology": c.symbology}})
        all_diffs.extend(dl)
        T.add("classify", time.perf_counter() - t0)

        v = masks.valid & ~band
        inst_out.append({
            "index": inst.index, "row": inst.row, "col": inst.col,
            "center_px": [round(inst.center[0], 1), round(inst.center[1], 1)],
            "angle_deg": round(inst.angle_deg, 3), "locate_score": round(inst.score, 3),
            "transform": {"h": [float(x) for x in (reg.H / reg.H[2, 2]).ravel()]},
            "registration_rms_px": None if not np.isfinite(reg.rms_px) else round(reg.rms_px, 3),
            "registration_tiles": reg.n_tiles,
            "mean_delta_e": round(float(maps.delta_e[v].mean()), 3) if v.any() else None,
            "folds": [{"p0_mm": [f.p0[0] / ppm, f.p0[1] / ppm], "p1_mm": [f.p1[0] / ppm, f.p1[1] / ppm],
                       "band_mm": round(f.band_px / ppm, 2)} for f in fl],
            "color_matrix": None if color_M is None else np.round(color_M["M"], 4).tolist(),
            "braille": {"read": b_reads, "stats": b_stats} if blines else None,
        })
        aligned_L.append(maps.smp_lab[..., 0].copy())
        bands.append(band)
        if spec["outputs"]["layers"]:
            _save_layers(layers_dir, inst.index, ref.rgb, aligned, dl, layer_scale)

    # ---------- text (PDF mot PDF) ----------
    text_diffs = []
    if kind == "pdf" and spec.get("text_compare", True) and states:
        t0 = time.perf_counter()
        rw = textdiff.extract_words(spots.sanitized_pdf, page, ref.geom)
        sw = textdiff.extract_words(sspots.sanitized_pdf, spec["sample"]["page"] - 1, srend.geom)
        text_diffs = textdiff.compare(rw, sw, states[0]["reg"].H, ppm)
        all_diffs[:] = _link_text(all_diffs, text_diffs, states[0]["inst"].index, ppm)
        T.add("text", time.perf_counter() - t0)

    # ---------- konsensus ----------
    t0 = time.perf_counter()
    if ispec.get("consensus") and len(aligned_L) >= 3:
        all_diffs.extend(_consensus(aligned_L, bands, masks.valid, all_diffs, insts, ppm, tol))
    # ---------- aggregering ----------
    agg = instances.aggregate(all_diffs, len(insts), ispec["systematic_ratio"])
    if expected and len(insts) < expected:
        agg.append({"instance": 0, "instances": [], "kind": "Missing", "severity": "Critical", "systematic": False,
                    "reason": "instance_not_found", "found": len(insts), "expected": expected,
                    "bbox_mm": None, "bbox_px": None, "objects": [], "metrics": {}, "confidence": 1.0})
    sev_rank = {s: i for i, s in enumerate(classify.SEV)}
    inst_folds = {io["index"]: io["folds"] for io in inst_out}
    for d in agg:
        d["certainty"] = certainty.assess(d, len(insts), tol, kind, inst_folds,
                                          color_calibrated=bool(spec.get("color_calibrated")))
    # säkrast först, sedan allvar
    agg.sort(key=lambda d: (certainty.LEVELS.index(d["certainty"]["level"]), -sev_rank[d["severity"]],
                            -d["certainty"]["score"], not d.get("systematic"), d["instances"][:1] or [0]))
    for i, d in enumerate(agg):
        d["id"] = i + 1
        d.pop("instance", None)
        d.pop("shift_px", None)
        if d.get("bbox_mm"):
            d["bbox_mm"] = [round(v, 3) for v in d["bbox_mm"]]
        if "area_mm2" in d:
            d["area_mm2"] = round(d["area_mm2"], 3)
    for io in inst_out:
        bad = [d for d in agg if io["index"] in d["instances"] and d["kind"] != "Uncertain"]
        io["status"] = "fail" if any(d["severity"] in ("Major", "Critical") for d in bad) else ("warn" if bad else "pass")
    T.add("aggregate", time.perf_counter() - t0)

    result = {
        "schema": "printcmp.result/1",
        "job_hash": "sha256:" + hashlib.sha256((_canonical({k: v for k, v in spec.items() if k != "outputs"}) +
                                                _sha256(spec["reference"]["path"]) +
                                                _sha256(spec["sample"]["path"]) + __version__).encode()).hexdigest(),
        "engine": {"printcmp": __version__, "renderer": renderer.name, "opencv": cv2.__version__},
        "reference": {"path": spec["reference"]["path"], "sha256": _sha256(spec["reference"]["path"]),
                      "size_mm": [round(ref.geom.width_mm, 3), round(ref.geom.height_mm, 3)], "dpi": dpi,
                      "dieline": masks.has_dieline, "spot_colors": spots.spot_names,
                      "barcodes": [{"symbology": c.symbology, "data": c.data,
                                    "bbox_mm": [round(v / ppm, 2) for v in c.bbox]} for c in ref_codes],
                      "objects": len(objs),
                      "objects_by_kind": {k: sum(1 for o in objs if o.kind == k) for k in sorted({o.kind for o in objs})},
                      "outlined_text_mm": [[round(v / ppm, 2) for v in o.bbox] for o in objs if o.kind == "OutlinedText"],
                      "tables": [t.to_json(ppm) for t in tables],
                      "layers": layer_report.get("reference"),
                      "braille": [{"line": ln.index, "text": ln.text, "source": ln.source,
                                   "unicode": "".join(braille.unicode_of(c) for c in ln.cell_list()),
                                   "cells": ln.kmax - ln.kmin + 1,
                                   "bbox_mm": [round(v / ppm, 2) for v in ln.bbox()],
                                   "dot_pitch_mm": round(ln.pitch / ppm, 2), "cell_pitch_mm": round(ln.cell_pitch / ppm, 2)}
                                  for ln in blines]},
        "sample": {"path": spec["sample"]["path"], "sha256": _sha256(spec["sample"]["path"]), "kind": kind,
                   "dpi": sample_dpi, "size_px": [int(sample_rgb.shape[1]), int(sample_rgb.shape[0])],
                   "screen_period_px": None if period is None else round(period, 2), "descreen_sigma_px": round(sigma, 2),
                   "extra_blur_px": round(extra_blur, 2)},
        "sheet": {"rotation_deg": round(loc_info["sheet_angle_deg"], 3), "match_score": round(loc_info["match_score"], 3),
                  "instances_found": len(insts), "instances_expected": expected},
        "tolerances": {"profile": spec["tolerance_profile"], **tol},
        "instances": inst_out,
        "differences": agg,
        "summary": _summary(agg, inst_out),
        "timing_ms": {k: int(v * 1000) for k, v in T.t.items()},
    }
    t0 = time.perf_counter()
    (out_dir / spec["outputs"]["json"]).write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding="utf-8")
    if spec["outputs"].get("report_html"):
        from . import report
        report.write_html(out_dir / spec["outputs"]["report_html"], result, ref.rgb, sample_rgb, insts, dpi,
                          sample_dpi, layers_dir if spec["outputs"]["layers"] else None)
    result["timing_ms"]["report"] = int((time.perf_counter() - t0) * 1000)
    result["timing_ms"]["total"] = sum(result["timing_ms"].values())
    (out_dir / spec["outputs"]["json"]).write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding="utf-8")
    prog("done", 1.0)
    return result


def _summary(diffs, insts):
    by_kind, by_sev = {}, {}
    for d in diffs:
        by_kind[d["kind"]] = by_kind.get(d["kind"], 0) + 1
        by_sev[d["severity"]] = by_sev.get(d["severity"], 0) + 1
    return {"differences": len(diffs), "by_kind": by_kind, "by_severity": by_sev,
            "systematic": sum(1 for d in diffs if d.get("systematic")),
            "instances_fail": sum(1 for i in insts if i["status"] == "fail"),
            "instances_warn": sum(1 for i in insts if i["status"] == "warn"),
            "instances_pass": sum(1 for i in insts if i["status"] == "pass")}


def _consensus(aligned_L, bands, valid, existing, insts, ppm, tol):
    """Jämför varje instans mot medianen av alla instanser med strängare tolerans."""
    # Medianen bildas av instanser utan veck (veckade instanser är sämre registrerade)
    clean = [L for L, b in zip(aligned_L, bands) if not b.any()]
    if len(clean) < 3:
        return []
    med = np.median(np.stack(clean, axis=0), axis=0).astype(np.float32)
    radius = int(round(tol["edge_radius_mm"] * ppm))
    thr = 0.75 * tol["struct_dl"]
    min_area = tol["min_blob_mm2"] * ppm * ppm
    out = []
    for L, band, inst in zip(aligned_L, bands, insts):
        d1 = diff._range_dist(L, med, radius)
        d2 = diff._range_dist(med, L, radius)
        dm = cv2.GaussianBlur(np.maximum(d1, d2), (0, 0), 0.7)
        m = ((dm > thr) & valid & ~band).astype(np.uint8)
        m = cv2.morphologyEx(m, cv2.MORPH_OPEN, np.ones((2, 2), np.uint8))
        # ta bort det som redan rapporterats för instansen
        for d in existing:
            if d["instance"] == inst.index and d.get("bbox_px"):
                x, y, w, h = d["bbox_px"]
                pad = int(ppm * 0.5)
                m[max(0, y - pad):y + h + pad, max(0, x - pad):x + w + pad] = 0
        n, labels, stats, _ = cv2.connectedComponentsWithStats(m, connectivity=8)
        for i in range(1, n):
            x, y, w, h, a = stats[i]
            if a < min_area:
                continue
            sub = labels[y:y + h, x:x + w] == i
            sdl = (L[y:y + h, x:x + w] - med[y:y + h, x:x + w])[sub]
            kind = "Missing" if (sdl > 0).mean() > 0.75 else ("Added" if (sdl > 0).mean() < 0.25 else "Shape")
            out.append({"instance": inst.index, "kind": kind, "severity": "Minor", "source": "consensus",
                        "bbox_px": [int(x), int(y), int(w), int(h)],
                        "bbox_mm": [x / ppm, y / ppm, w / ppm, h / ppm], "area_mm2": a / ppm ** 2,
                        "objects": [], "metrics": {"delta_l": round(float(dm[y:y + h, x:x + w][sub].max()), 2),
                                                   "delta_e": None, "shift_mm": None},
                        "confidence": 0.7})
    # En konsensusavvikelse ska vara lokal: finns den i många instanser är det medianen
    # (eller registreringen) som avviker, inte trycket.
    keep = []
    for d in out:
        cx = d["bbox_mm"][0] + d["bbox_mm"][2] / 2
        cy = d["bbox_mm"][1] + d["bbox_mm"][3] / 2
        n_same = sum(1 for e in out if abs(e["bbox_mm"][0] + e["bbox_mm"][2] / 2 - cx) < 1.0
                     and abs(e["bbox_mm"][1] + e["bbox_mm"][3] / 2 - cy) < 1.0)
        if n_same <= max(1, int(0.25 * len(aligned_L))):
            keep.append(d)
    return keep


KIND_COLORS = {  # RGB
    "Missing": (220, 38, 38), "Added": (234, 88, 12), "Changed": (192, 38, 211), "Moved": (37, 99, 235),
    "Color": (22, 163, 74), "Shape": (147, 51, 234), "Barcode": (190, 18, 60), "Uncertain": (148, 163, 184),
    "Braille": (13, 148, 136),
}


def _save_layers(layers_dir: Path, idx: int, ref_rgb, aligned, diffs, scale):
    def sc(img):
        return cv2.resize(img, None, fx=scale, fy=scale, interpolation=cv2.INTER_AREA) if scale < 1 else img

    r, s = sc(ref_rgb), sc(aligned)
    ov = s.copy()
    for d in diffs:
        if not d.get("bbox_px"):
            continue
        x, y, w, h = (int(round(v * scale)) for v in d["bbox_px"])
        c = KIND_COLORS.get(d["kind"], (0, 0, 0))
        cv2.rectangle(ov, (x - 3, y - 3), (x + w + 3, y + h + 3), c, 2)
    imageio.write_png(layers_dir / f"inst{idx:03d}_ref.png", r)
    imageio.write_png(layers_dir / f"inst{idx:03d}_sample.png", s)
    imageio.write_png(layers_dir / f"inst{idx:03d}_overlay.png", ov)


def _line_params(p0, p1, center):
    """Vinkel (grader, 0–180) och signerat avstånd från center till linjen."""
    p0, p1 = np.asarray(p0, float), np.asarray(p1, float)
    d = p1 - p0
    ang = float(np.degrees(np.arctan2(d[1], d[0])) % 180)
    a = np.radians(ang)
    n = np.array([-np.sin(a), np.cos(a)])
    return ang, float(np.dot(p0 - np.asarray(center, float), n))


def _drop_systematic_fold_candidates(states, ref_shape, ppm, sppm):
    """Ta bort veckkandidater som återkommer på samma ställe i många etiketter.

    En ändring i originalet (t.ex. en flyttad textrad) ger en rak kant i residualen på
    samma etikettposition i alla instanser. Ett verkligt veck är en rak linje i arkets
    koordinater: återkommer det i flera etiketter ligger träffarna på en och samma
    arklinje. Kandidater som återkommer i minst tre etiketter utan att vara kolinjära
    på arket är alltså ändringar i originalet, inte veck.
    """
    h_ref, w_ref = ref_shape
    c_lab = (w_ref / 2, h_ref / 2)
    items = []      # (state index, fold, label-ang, label-rho, sheet-ang, sheet-rho)
    for i, st in enumerate(states):
        H = st["reg"].H
        for f in st["fl"]:
            a_l, r_l = _line_params(f.p0, f.p1, c_lab)
            q = np.array([[*f.p0, 1.0], [*f.p1, 1.0]]) @ H.T
            q = q[:, :2] / q[:, 2:3]
            a_s, r_s = _line_params(q[0], q[1], (0.0, 0.0))
            items.append((i, f, a_l, r_l, a_s, r_s))
    n_inst = len(states)
    need = max(3, int(np.ceil(0.4 * n_inst)))
    drop = set()
    for k, (i, f, a_l, r_l, a_s, r_s) in enumerate(items):
        same = []
        for j, (i2, f2, a2, r2, as2, rs2) in enumerate(items):
            da = abs(a2 - a_l)
            da = min(da, 180 - da)
            if da < 8 and abs(r2 - r_l) < 2.5 * ppm:
                same.append(j)
        insts = {items[j][0] for j in same}
        if len(insts) < need:
            continue
        # kolinjära på arket? (vinkel nära 0/180 kan byta tecken på avståndet)
        rs = []
        for j in same:
            a2, r2 = items[j][4], items[j][5]
            rs.append(-r2 if abs(a2 - a_s) > 90 else r2)
        if np.ptp(rs) > 3.0 * sppm:
            drop.update(same)
    for i, st in enumerate(states):
        st["fl"] = [f for k, (i2, f, *_r) in enumerate(items) if i2 == i and k not in drop]


def _layer_json(info) -> dict:
    return {"ocgs": info.ocgs, "spots": info.spots}


TEXT_KIND_SV = {"Changed": "ändrad", "Missing": "borttagen", "Added": "tillagd", "Moved": "flyttad", "Style": "typsnitt/storlek"}


def _link_text(diffs: list[dict], tdiffs: list[dict], instance: int, ppm: float) -> list[dict]:
    """Koppla textlagrets skillnader till de grafiska.

    En grafisk skillnad som överlappar en textskillnad får texten (”10 %” -> ”18 %”).
    En textskillnad utan grafisk motsvarighet rapporteras separat: den syns inte i tryck
    (t.ex. fel i ToUnicode-tabellen, vit/dold text) men påverkar sökbarhet och tillgänglighet.
    """
    def ov(a, b, pad=0.3):
        ax, ay, aw, ah = a
        bx, by, bw, bh = b
        return not (ax - pad > bx + bw or bx - pad > ax + aw or ay - pad > by + bh or by - pad > ay + ah)

    used = set()
    for d in diffs:
        if d.get("instance") != instance or not d.get("bbox_mm") or d["kind"] == "Uncertain":
            continue
        hits = [k for k, t in enumerate(tdiffs) if ov(d["bbox_mm"], t["bbox_mm"])]
        if hits:
            d["text_change"] = [tdiffs[k]["text"] | {"kind": tdiffs[k]["kind"]} for k in hits[:6]]
            if d.get("category") in (None, "Background", "Vector", "Image"):
                d["category"] = "Text"
            # en textändring som skjuter resten av raden är en ändring, inte en flytt
            if d["kind"] in ("Moved", "Shape") and any(tdiffs[k]["kind"] in ("Changed", "Added", "Missing")
                                                       for k in hits):
                d["kind"] = "Changed"
            used.update(hits)
    out = list(diffs)
    for k, t in enumerate(tdiffs):
        if k in used:
            continue
        # syns inte grafiskt
        visible_kinds = {"Style": "Minor"}
        out.append({"instance": instance, "kind": "Changed" if t["kind"] == "Style" else t["kind"],
                    "severity": visible_kinds.get(t["kind"], "Minor"), "category": "TextLayer", "source": "text",
                    "bbox_px": t["bbox_px"], "bbox_mm": t["bbox_mm"],
                    "area_mm2": t["bbox_mm"][2] * t["bbox_mm"][3], "objects": [],
                    "metrics": {"delta_l": None, "delta_e": None, "shift_mm": t["text"].get("shift_mm")},
                    "confidence": 0.9, "text_change": [t["text"] | {"kind": t["kind"]}],
                    "text_only": True})
    return out

