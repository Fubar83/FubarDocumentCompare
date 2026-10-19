"""Utvärdering mot facit (avsnitt 10).

    python -m tools.eval.evaluate CASES_DIR [--run] [--json metrics.json]

--run kör pipelinen på varje fall först (annars läses befintlig out/result.json).
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from collections import defaultdict
from pathlib import Path

import numpy as np
from scipy.optimize import linear_sum_assignment

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from printcmp import jobspec, pipeline  # noqa: E402


CLASS_OK = {
    "char_change": {"Changed", "Shape", "Moved"},      # bredare/smalare tecken flyttar resten av raden
    "outlined_change": {"Changed", "Shape", "Moved"},
    "diacritic": {"Changed", "Missing", "Shape"},
    "word_delete": {"Missing", "Changed", "Moved"},
    "append_text": {"Added", "Changed"},
    "font_size": {"Shape", "Changed"},
    "move": {"Moved"},
    "delete": {"Missing"},
    "color_shift": {"Color"},
    "barcode_digit": {"Barcode"},
    "cell_value": {"Changed", "Shape"},
    "rule_delete": {"Missing"},
    "row_shift": {"Moved"},
    "braille_char": {"Braille"},
}


def _expand(b, pad):
    x, y, w, h = b
    return [x - pad, y - pad, w + 2 * pad, h + 2 * pad]


def _iou(a, b):
    ax0, ay0, ax1, ay1 = a[0], a[1], a[0] + a[2], a[1] + a[3]
    bx0, by0, bx1, by1 = b[0], b[1], b[0] + b[2], b[1] + b[3]
    ix = max(0.0, min(ax1, bx1) - max(ax0, bx0))
    iy = max(0.0, min(ay1, by1) - max(ay0, by0))
    inter = ix * iy
    u = a[2] * a[3] + b[2] * b[3] - inter
    return inter / u if u > 0 else 0.0


def _contains(b, p):
    return b[0] <= p[0] <= b[0] + b[2] and b[1] <= p[1] <= b[1] + b[3]


def _center(b):
    return (b[0] + b[2] / 2, b[1] + b[3] / 2)


def match_score(det_box, truth_box, pad=0.3):
    t = _expand(truth_box, pad)
    d = _expand(det_box, pad)
    iou = _iou(d, t)
    if iou >= 0.1 or _contains(t, _center(det_box)) or _contains(d, _center(truth_box)):
        return max(iou, 0.1)
    return 0.0


def _near_fold(box, lines, band):
    cx, cy = _center(box)
    for ln in lines:
        p0, p1 = np.array(ln["p0_mm"]), np.array(ln["p1_mm"])
        d = p1 - p0
        t = np.dot(np.array([cx, cy]) - p0, d) / max(np.dot(d, d), 1e-9)
        dist = np.linalg.norm(p0 + t * d - np.array([cx, cy]))
        if dist <= max(band, ln.get("band_mm", 0)) + max(box[2], box[3]) / 2:
            return True
    return False


def category_ok(t: dict, d: dict) -> bool:
    """Rätt objektkategori (och tabellcell/-linje) för en träff."""
    c = t["category"]
    ti = d.get("table") or {}
    if c == "TableCell":
        if ti.get("element") != "cell":
            return False
        r, col = t["table_cell"]
        return ti.get("row") == r and (col is None or ti.get("col") in (col, None))
    if c == "TableRule":
        return ti.get("element") == "rule" and ti.get("position") == t.get("table_rule")
    if c == "OutlinedText":
        return d.get("category") == "OutlinedText" and d.get("severity") in ("Major", "Critical")
    if c == "Braille":
        return d.get("kind") == "Braille"
    return d.get("category") == c


def homography_rms(H_res, H_truth, w, h, n=8):
    xs, ys = np.meshgrid(np.linspace(0, w, n), np.linspace(0, h, n))
    P = np.stack([xs.ravel(), ys.ravel(), np.ones(n * n)], 1)
    a = P @ np.asarray(H_res).reshape(3, 3).T
    b = P @ np.asarray(H_truth).reshape(3, 3).T
    a = a[:, :2] / a[:, 2:3]
    b = b[:, :2] / b[:, 2:3]
    return float(np.sqrt(np.mean(np.sum((a - b) ** 2, 1))))


def evaluate_case(result: dict, truth: dict) -> dict:
    dpi = truth["dpi"]
    W, H = truth["label_size_mm"]
    # instansmappning via rad/kolumn
    t_by_rc = {(i["row"], i["col"]): i for i in truth["instances"]}
    r_by_rc = {(i["row"], i["col"]): i for i in result["instances"]}
    inst_map = {}       # resultat-index -> facit-index
    reg_err = []
    for rc, ti in t_by_rc.items():
        ri = r_by_rc.get(rc)
        if ri is None:
            continue
        inst_map[ri["index"]] = ti["index"]
        if ti.get("folds"):
            continue          # en transform kan inte beskriva ett veckat etikett-område
        reg_err.append(homography_rms(ri["transform"]["h"], ti["transform_ref_to_sample"],
                                      W * dpi / 25.4, H * dpi / 25.4))
    t_instances = [i["index"] for i in truth["instances"]]
    folds_by_inst = {u["instance"]: u["lines"] for u in truth.get("expect_uncertain", [])}

    # expandera detektioner per (facit-)instans
    dets = []
    for d in result["differences"]:
        if not d.get("bbox_mm"):
            continue
        for ri in d["instances"]:
            if ri in inst_map:
                dets.append({"inst": inst_map[ri], "box": d["bbox_mm"], "kind": d["kind"],
                             "systematic": bool(d.get("systematic")), "id": d["id"], "severity": d["severity"],
                             "d": d})
    truths = []
    for t in truth["defects"]:
        insts = t_instances if t["systematic"] else t["instances"]
        for ti in insts:
            truths.append({"inst": ti, "box": t["bbox_ref_mm"], "kind": t["kind"], "accept": t["accept"],
                           "systematic": t["systematic"], "id": t["id"], "mutation": t.get("mutation"),
                           "origin": "artwork" if t["systematic"] else "print", "category": t.get("category"),
                           "table_cell": t.get("table_cell"), "table_rule": t.get("table_rule")})

    per_inst_hits = []
    fa, uncertain_ok, uncertain_extra = [], 0, 0
    for ti in t_instances:
        D = [d for d in dets if d["inst"] == ti]
        T = [t for t in truths if t["inst"] == ti]
        S = np.zeros((len(T), len(D)))
        for a, t in enumerate(T):
            for b, d in enumerate(D):
                if d["kind"] == "Uncertain" and not t["systematic"]:
                    continue
                S[a, b] = match_score(d["box"], t["box"]) if (d["kind"] in t["accept"] or d["kind"] == "Uncertain") else 0.0
        matched_d = set()
        if len(T) and len(D):
            rows, cols = linear_sum_assignment(-S)
            for a, b in zip(rows, cols):
                if S[a, b] > 0:
                    per_inst_hits.append((T[a], D[b]))
                    matched_d.add(b)
        hit_t = {id(t) for t, _ in per_inst_hits}
        for t in T:
            if id(t) not in hit_t:
                per_inst_hits.append((t, None))
        # överskott: en detektion som överlappar ett redan träffat facit räknas inte som falsklarm
        for b, d in enumerate(D):
            if b in matched_d:
                continue
            if any(match_score(d["box"], t["box"], pad=0.6) > 0 for t in T):
                continue
            if d["kind"] == "Uncertain":
                if _near_fold(d["box"], folds_by_inst.get(ti, []), 3.0):
                    uncertain_ok += 1
                else:
                    uncertain_extra += 1
                continue
            fa.append(d)

    by_origin = defaultdict(lambda: [0, 0])
    by_mut = defaultdict(lambda: [0, 0])
    cls_ok = cls_n = sys_ok = sys_n = 0
    cls_misses = []
    by_cat = defaultdict(lambda: [0, 0])
    cat_misses = []
    misses = []
    for t, d in per_inst_hits:
        hit = d is not None and d["kind"] != "Uncertain"
        by_origin[t["origin"]][1] += 1
        by_mut[t["mutation"]][1] += 1
        if hit:
            by_origin[t["origin"]][0] += 1
            by_mut[t["mutation"]][0] += 1
            if t["origin"] == "artwork":
                cls_n += 1
                ok_c = d["kind"] in CLASS_OK.get(t["mutation"], {t["kind"]})
                cls_ok += int(ok_c)
                if not ok_c:
                    cls_misses.append({"id": t["id"], "mutation": t["mutation"], "got": d["kind"]})
            sys_n += 1
            sys_ok += int(d["systematic"] == t["systematic"])
            if t.get("category"):
                ok = category_ok(t, d["d"])
                by_cat[t["category"]][1] += 1
                by_cat[t["category"]][0] += int(ok)
                if not ok:
                    cat_misses.append({"id": t["id"], "category": t["category"], "got": d["d"].get("category"),
                                       "table": d["d"].get("table"), "kind": d["kind"]})
        else:
            misses.append({"id": t["id"], "inst": t["inst"], "mutation": t["mutation"], "box": t["box"],
                           "got": None if d is None else d["kind"]})
    n_inst = len(t_instances)
    return {
        "instances_expected": n_inst,
        "instances_found": result["sheet"]["instances_found"],
        "instances_mapped": len(inst_map),
        "registration_rms_px": float(np.mean(reg_err)) if reg_err else None,
        "registration_rms_px_max": float(np.max(reg_err)) if reg_err else None,
        "recall_artwork": by_origin["artwork"],
        "recall_print": by_origin["print"],
        "recall_by_mutation": dict(by_mut),
        "false_alarms": len(fa),
        "false_alarms_per_label": len(fa) / max(1, n_inst),
        "false_alarm_kinds": dict(_count(d["kind"] for d in fa)),
        "uncertain_ok": uncertain_ok,
        "uncertain_extra": uncertain_extra,
        "class_ok": [cls_ok, cls_n],
        "class_misses": cls_misses,
        "category_ok": {k: v for k, v in by_cat.items()},
        "category_misses": cat_misses[:20],
        "systematic_ok": [sys_ok, sys_n],
        "misses": misses,
        "false_alarm_list": [{"id": d["id"], "inst": d["inst"], "kind": d["kind"],
                              "box": [round(v, 2) for v in d["box"]]} for d in fa][:50],
    }


def _count(it):
    c = defaultdict(int)
    for x in it:
        c[x] += 1
    return c


def aggregate(results: list[dict]) -> dict:
    def ratio(key):
        a = sum(r[key][0] for r in results)
        b = sum(r[key][1] for r in results)
        return {"hit": a, "total": b, "rate": (a / b) if b else None}

    muts = defaultdict(lambda: [0, 0])
    cats = defaultdict(lambda: [0, 0])
    for r in results:
        for k, (a, b) in r["recall_by_mutation"].items():
            muts[k][0] += a
            muts[k][1] += b
        for k, (a, b) in r.get("category_ok", {}).items():
            cats[k][0] += a
            cats[k][1] += b
    labels = sum(r["instances_expected"] for r in results)
    reg = [r["registration_rms_px"] for r in results if r["registration_rms_px"] is not None]
    return {
        "cases": len(results),
        "labels": labels,
        "instances_found_rate": sum(min(r["instances_found"], r["instances_expected"]) for r in results) / max(1, labels),
        "recall_artwork": ratio("recall_artwork"),
        "recall_print": ratio("recall_print"),
        "classification": ratio("class_ok"),
        "systematic_flag": ratio("systematic_ok"),
        "false_alarms_per_label": sum(r["false_alarms"] for r in results) / max(1, labels),
        "uncertain_extra_per_label": sum(r["uncertain_extra"] for r in results) / max(1, labels),
        "registration_rms_px_mean": float(np.mean(reg)) if reg else None,
        "recall_by_mutation": {k: {"hit": a, "total": b, "rate": a / b if b else None} for k, (a, b) in sorted(muts.items())},
        "category": {k: {"hit": a, "total": b, "rate": a / b if b else None} for k, (a, b) in sorted(cats.items())},
    }


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("cases")
    ap.add_argument("--run", action="store_true")
    ap.add_argument("--json", default=None)
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args(argv)
    out = []
    for case in sorted(Path(a.cases).glob("case_*")):
        if a.run:
            t0 = time.time()
            spec = jobspec.load(case / "jobspec.json")
            pipeline.run(spec)
            dt = time.time() - t0
        else:
            dt = None
        res = json.loads((case / "out" / "result.json").read_text(encoding="utf-8"))
        truth = json.loads((case / "truth.json").read_text(encoding="utf-8"))
        ev = evaluate_case(res, truth)
        ev["case"] = case.name
        ev["seconds"] = dt
        out.append(ev)
        ra, rp = ev["recall_artwork"], ev["recall_print"]
        print(f"{case.name}: inst {ev['instances_found']}/{ev['instances_expected']}  "
              f"artwork {ra[0]}/{ra[1]}  print {rp[0]}/{rp[1]}  FA {ev['false_alarms']} "
              f"({ev['false_alarms_per_label']:.2f}/etikett)  osäkra extra {ev['uncertain_extra']}  "
              f"reg {ev['registration_rms_px'] or float('nan'):.2f}px"
              + (f"  {dt:.0f}s" if dt else ""), flush=True)
        if a.verbose:
            for m in ev["misses"]:
                print("   MISS", m)
            for f in ev["false_alarm_list"][:15]:
                print("   FA  ", f)
    agg = aggregate(out)
    print(json.dumps(agg, indent=2, ensure_ascii=False))
    if a.json:
        Path(a.json).write_text(json.dumps({"aggregate": agg, "cases": out}, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
