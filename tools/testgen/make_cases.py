"""Generera testfall: referens-PDF, simulerat scannat ark, JobSpec och facit.

    python -m tools.testgen.make_cases --out cases --n 6 --seed 100 --difficulty normal
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import cv2
import numpy as np
import pypdfium2 as pdfium

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from printcmp import imageio, pdfspots  # noqa: E402
from tools.testgen import labelpdf, mutate, scanner, sim  # noqa: E402
from tools.testgen.templates import TEMPLATES  # noqa: E402

GENERATOR_VERSION = "testgen 0.3.0"

DIFFICULTY = {
    "easy":   {"halftone": False, "rot": 1.0, "fold_p": 0.0, "noise": (0.8, 1.5), "mtf": (0.6, 0.8),
               "misreg": 0.0, "gain": (0.04, 0.08), "inst_rot180_p": 0.0, "emboss": (0.18, 0.28),
               "emboss_missing_p": 0.5, "lift": (0.1, 0.3)},
    "normal": {"halftone": True, "rot": 2.0, "fold_p": 0.5, "noise": (1.0, 2.5), "mtf": (0.7, 1.0),
               "misreg": 0.06, "gain": (0.06, 0.16), "inst_rot180_p": 0.0, "emboss": (0.12, 0.22),
               "emboss_missing_p": 0.5, "lift": (0.1, 0.6)},
    "hard":   {"halftone": True, "rot": 3.5, "fold_p": 0.9, "noise": (2.0, 3.5), "mtf": (0.9, 1.2),
               "misreg": 0.12, "gain": (0.10, 0.22), "inst_rot180_p": 0.15, "emboss": (0.08, 0.16),
               "emboss_missing_p": 0.6, "lift": (0.2, 1.2)},
}


def render_rgb(pdf_bytes, dpi):
    clean = pdfspots.analyze(pdf_bytes).sanitized_pdf
    doc = pdfium.PdfDocument(clean)
    try:
        bm = doc[0].render(scale=dpi / 72.0, rev_byteorder=True, fill_color=(255, 255, 255, 255))
        a = bm.to_numpy()[:, :, :3]
        return np.ascontiguousarray(a)
    finally:
        doc.close()


def make_case(template: str, seed: int, out_dir: Path, difficulty: str = "normal", cols: int = 2, rows: int = 4,
              allowed=None,
              n_artwork=None, n_print=None, dpi: int = 600, scanner_kind: str = "legacy",
              fold_lift=None, fold_p=None):
    rng = np.random.default_rng(seed)
    D = DIFFICULTY[difficulty]
    out_dir.mkdir(parents=True, exist_ok=True)
    base = TEMPLATES[template]()
    W, H = base["size_mm"]
    ppm = dpi / 25.4

    ref_pdf = labelpdf.render_pdf(base)
    (out_dir / "reference.pdf").write_bytes(ref_pdf)

    na = int(rng.integers(0, 4)) if n_artwork is None else n_artwork
    mutated, art_defects = mutate.mutate(base, rng, na, allowed)
    mut_pdf = labelpdf.render_pdf(mutated)

    # ---- tryck ----
    p = {
        "halftone": D["halftone"],
        "lpcm": float(rng.uniform(60, 80)),
        "dot_gain": {n: float(rng.uniform(*D["gain"])) for n in "CMYK"},
        "density": {n: float(rng.uniform(0.95, 1.05)) for n in "CMYK"},
        "misreg_mm": {n: [float(v) for v in rng.uniform(-D["misreg"], D["misreg"], 2)] for n in "CMYK"},
    }
    p["misreg_mm"]["K"] = [0.0, 0.0]
    dpi_hi = 1200 if D["halftone"] else dpi
    label_hi = render_rgb(mut_pdf, dpi_hi)
    label = sim.print_label(label_hi, dpi_hi, dpi, p, rng)        # float32, dpi
    lh, lw = label.shape[:2]
    die = sim.rounded_rect_mask(lw, lh, mutated["dieline"]["radius_mm"] * ppm)

    var_el = next((e for e in base["elements"] if e["type"] == "variable"), None)
    var_box = var_el["rect_mm"] if var_el else None

    # ---- ark ----
    gap, margin = 4.0, 12.0
    sw_mm = cols * W + (cols - 1) * gap + 2 * margin
    sh_mm = rows * H + (rows - 1) * gap + 2 * margin
    SW, SH = int(round(sw_mm * ppm)), int(round(sh_mm * ppm))
    backing = np.array([0.93, 0.925, 0.86], np.float32)
    sheet = np.empty((SH, SW, 3), np.float32)
    sheet[:] = backing
    print_defects, instances = [], []
    np_total = int(rng.integers(0, 5)) if n_print is None else n_print
    defect_inst = set(rng.choice(rows * cols, size=min(np_total, rows * cols), replace=False).tolist()) if np_total else set()
    avoid = [d["bbox_ref_mm"] for d in art_defects] + ([var_box] if var_box else [])
    # punktskrift: präglade punkter (från den muterade layouten) per instans
    b_els = [e for e in mutated["elements"] if e["type"] == "braille"]
    b_dots = [(x, y, e["id"], ci, n, e.get("dot_d_mm", 1.5) / 2) for e in b_els
              for (x, y, ci, n) in labelpdf.braille_dots_mm(e)]
    avoid += [labelpdf.element_bbox_mm(e) for e in b_els]
    emb_strength = float(rng.uniform(*D["emboss"]))
    emb_light = (0.0, -1.0) if rng.random() < 0.5 else (0.0, 1.0)
    smodel = None
    if scanner_kind != "legacy":
        # scannermodellen bestämmer ljusriktning och präglingens skuggstyrka
        smodel = scanner.make_model(scanner_kind, np.random.default_rng(seed * 7919 + 13))
        emb_strength *= smodel.emboss_gain
        emb_light = (0.0, smodel.light_sign)
    emb_missing = {}
    if b_dots and rng.random() < D["emboss_missing_p"]:
        k = int(rng.integers(rows * cols)) + 1
        emb_missing[k] = int(rng.integers(len(b_dots)))
    idx = 0
    for r in range(rows):
        for c in range(cols):
            idx += 1
            img = label.copy()
            dens = float(rng.uniform(0.97, 1.03))
            img = np.clip(sim.PAPER - (sim.PAPER - img) * dens, 0, 1)
            if (idx - 1) in defect_inst:
                kind = ["spot", "streak", "hickey"][int(rng.integers(0, 3))]
                res = sim.add_print_defect(img, ppm, kind, rng, (2.5, 2.5, W - 2.5, H - 2.5), avoid)
                if res:
                    box, pol, st = res
                    print_defects.append({"id": f"p{len(print_defects) + 1}", "kind": "Spot" if kind == "spot" else kind.capitalize(),
                                          "accept": mutate.STRUCT_ANY, "systematic": False, "instances": [idx],
                                          "bbox_ref_mm": [round(v, 3) for v in box], "polarity": pol,
                                          "strength": round(st, 3), "mutation": kind})
            if var_el:
                x, y, w, h = var_el["rect_mm"]
                x0, y0, x1, y1 = (int(round(v * ppm)) for v in (x, y, x + w, y + h))
                # sudda platshållartexten med etikettens bakgrund (median av en ring runt rutan)
                ring = np.concatenate([img[max(0, y0 - 3):y0, x0:x1].reshape(-1, 3),
                                       img[y1:y1 + 3, x0:x1].reshape(-1, 3)])
                img[y0:y1, x0:x1] = np.median(ring, axis=0) if len(ring) else sim.PAPER
                txt = f"LOT {int(rng.integers(1000, 9999))}-{idx:04d}"
                tm = np.zeros((y1 - y0, x1 - x0), np.uint8)
                cv2.putText(tm, txt, (4, tm.shape[0] - int(0.7 * ppm)), cv2.FONT_HERSHEY_SIMPLEX,
                            0.085 * ppm / 2.0, 255, max(1, int(0.12 * ppm)), cv2.LINE_AA)
                a_ = (tm.astype(np.float32) / 255.0)[..., None]
                img[y0:y1, x0:x1] = img[y0:y1, x0:x1] * (1 - a_) + np.float32(0.1) * a_
            if b_dots:
                keep = [d for j, d in enumerate(b_dots) if emb_missing.get(idx) != j]
                sim.emboss(img, [(x * ppm, y * ppm) for x, y, *_ in keep], b_dots[0][5] * ppm, emb_strength, emb_light)
                if idx in emb_missing:
                    x, y, eid, ci, n, rr = b_dots[emb_missing[idx]]
                    print_defects.append({"id": f"p{len(print_defects) + 1}", "kind": "Braille", "category": "Braille",
                                          "accept": ["Braille"], "systematic": False, "instances": [idx],
                                          "bbox_ref_mm": [round(x - rr - 0.3, 3), round(y - rr - 0.3, 3),
                                                          round(2 * rr + 0.6, 3), round(2 * rr + 0.6, 3)],
                                          "mutation": "emboss_missing", "detail": {"cell": ci, "dot": n}})
            rot180 = rng.random() < D["inst_rot180_p"]
            if rot180:
                img = img[::-1, ::-1].copy()
                dmask = die[::-1, ::-1]
            else:
                dmask = die
            ox = margin + c * (W + gap) + float(rng.uniform(-0.3, 0.3))
            oy = margin + r * (H + gap) + float(rng.uniform(-0.3, 0.3))
            ox_px, oy_px = int(round(ox * ppm)), int(round(oy * ppm))
            region = sheet[oy_px:oy_px + lh, ox_px:ox_px + lw]
            region[dmask] = img[dmask]
            # stanskant: tunn skugga
            edge = cv2.morphologyEx(dmask.astype(np.uint8), cv2.MORPH_GRADIENT, np.ones((3, 3), np.uint8)).astype(bool)
            region[edge] *= 0.82
            # placering: label px -> ark px
            if rot180:
                A = np.array([[-1, 0, ox_px + lw - 1], [0, -1, oy_px + lh - 1], [0, 0, 1]], np.float64)
            else:
                A = np.array([[1, 0, ox_px], [0, 1, oy_px], [0, 0, 1]], np.float64)
            instances.append({"index": idx, "row": r, "col": c, "rotation": 180 if rot180 else 0, "A": A})

    # ---- arkgeometri: rotation + skala + förskjutning ----
    theta = float(rng.uniform(-D["rot"], D["rot"]))
    scale = float(1 + rng.uniform(-0.002, 0.002))
    shift = rng.uniform(-2, 2, 2) * ppm
    G = cv2.getRotationMatrix2D((SW / 2, SH / 2), theta, scale)
    G[:, 2] += shift
    lid = np.array([0.97, 0.97, 0.97], np.float32)
    G3 = np.vstack([G, [0, 0, 1]])
    folds = []
    geo, dust = None, []
    fp = D["fold_p"] if fold_p is None else fold_p
    mtf = tuple(float(v) for v in rng.uniform(*D["mtf"], 2))
    noise = float(rng.uniform(*D["noise"]))

    if smodel is None:
        # ---- simulator v1: affin placering, geometriskt veck, enkel scanner ----
        scan = cv2.warpAffine(sheet, G, (SW, SH), flags=cv2.INTER_CUBIC, borderMode=cv2.BORDER_CONSTANT,
                              borderValue=tuple(float(v) for v in lid))
        del sheet
        if rng.random() < fp:
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
            wpx = float(rng.uniform(0.6, 1.6) * ppm)
            st = float(rng.uniform(0.06, 0.2))
            amp = float(rng.uniform(0.05, 0.25) * ppm)   # tillplattat veck: förskjutning per sida
            scan = sim.fold(scan, p0, p1, wpx, st, amp)
            folds.append({"p0_px": [float(p0[0]), float(p0[1])], "p1_px": [float(p1[0]), float(p1[1])],
                          "width_mm": round(wpx / ppm, 3), "strength": round(st, 3), "amp_mm": round(amp / ppm, 3),
                          "band_mm": round(wpx / ppm * 2.5, 3)})
        scan_u8 = sim.scanner(scan, rng, mtf, noise)
        del scan
    else:
        # ---- simulator v2: flatbäddsscanner (CIS/CCD) med fysiskt veck ----
        geo_folds = []
        if rng.random() < fp:
            lr = (fold_lift, fold_lift) if fold_lift is not None else D["lift"]
            f = scanner.random_fold(rng, SW, SH, ppm, lift_range=lr)
            geo_folds.append(f)
            if f.crack:
                scanner.crack_ink(sheet, G, f, ppm, rng, sim.PAPER)
            folds.append({"p0_px": list(f.p0), "p1_px": list(f.p1), "kind": f.kind, "lift_mm": round(f.lift_mm, 3),
                          "width_mm": round(f.width_mm, 3), "crack": f.crack, "band_mm": round(2 * f.band_mm(), 3)})
        scan_u8, geo, dust = scanner.scan(sheet, G, geo_folds, smodel, rng, ppm, mtf, noise,
                                          lid=tuple(float(v) for v in lid))
        del sheet
    imageio.write_tiff(out_dir / "sample.tif", scan_u8, dpi)

    # ---- facit ----
    inst_truth = []
    expect_uncertain = []
    scan_artifacts = []
    for inst in instances:
        Hm = G3 @ inst["A"]                       # label px -> scan px
        if geo is not None:
            # v2: bästa affina approximation av den verkliga avbildningen (stegmotor, perspektiv,
            # förkortning) över etikettens yta
            gx, gy = np.meshgrid(np.linspace(0, lw, 9), np.linspace(0, lh, 9))
            P = np.stack([gx.ravel(), gy.ravel(), np.ones(gx.size)], 1)
            S_ = P @ inst["A"].T
            qx, qy = geo.to_scan(S_[:, 0], S_[:, 1], G)
            sol, *_ = np.linalg.lstsq(P, np.stack([qx, qy], 1), rcond=None)
            Hm = np.vstack([sol.T, [0, 0, 1]])
        # v1: vecket förskjuter hela arkhalvan: lägg in förskjutningen vid etikettens mitt i facit
        for f in (folds if geo is None else []):
            c = Hm @ np.array([lw / 2, lh / 2, 1.0])
            c = c[:2] / c[2]
            p0, p1 = np.array(f["p0_px"]), np.array(f["p1_px"])
            dvec = p1 - p0
            nvec = np.array([-dvec[1], dvec[0]]) / np.linalg.norm(dvec)
            s = float(np.dot(c - p0, nvec))
            wpx = f["width_mm"] * ppm
            dd = f["amp_mm"] * ppm * np.tanh(s / (3 * wpx))
            Hm = np.array([[1, 0, dd * nvec[0]], [0, 1, dd * nvec[1]], [0, 0, 1]]) @ Hm
        rec = {"index": inst["index"], "row": inst["row"], "col": inst["col"], "rotation": inst["rotation"],
               "transform_ref_to_sample": [float(v) for v in Hm.ravel()]}
        Hinv = np.linalg.inv(Hm)
        fl = []
        for f in folds:
            pts = np.array([f["p0_px"] + [1], f["p1_px"] + [1]]) @ Hinv.T
            pts = pts[:, :2] / pts[:, 2:3] / ppm
            # skär vecket etiketten?
            m_ = f["band_mm"] / 2
            if _line_hits_rect(pts[0], pts[1], (-m_, -m_, W + m_, H + m_)):
                fl.append({"p0_mm": pts[0].round(3).tolist(), "p1_mm": pts[1].round(3).tolist(),
                           "band_mm": f["band_mm"]})
        if fl:
            expect_uncertain.append({"instance": inst["index"], "cause": "fold", "lines": fl})
        rec["folds"] = fl
        inst_truth.append(rec)
        for bx, by, bw, bh in dust:
            c_ = Hinv @ np.array([bx + bw / 2, by + bh / 2, 1.0])
            cx_, cy_ = c_[0] / c_[2] / ppm, c_[1] / c_[2] / ppm
            if -1 <= cx_ <= W + 1 and -1 <= cy_ <= H + 1:
                r_ = max(bw, bh) / 2 / ppm + 0.1
                scan_artifacts.append({"instance": inst["index"], "kind": "dust",
                                       "bbox_ref_mm": [round(cx_ - r_, 3), round(cy_ - r_, 3), round(2 * r_, 3),
                                                       round(2 * r_, 3)]})

    truth = {
        "schema": "printcmp.truth/1", "seed": seed, "generator": GENERATOR_VERSION, "template": template,
        "difficulty": difficulty, "reference": "reference.pdf", "sample": "sample.tif", "dpi": dpi,
        "label_size_mm": [W, H],
        "sheet": {"rows": rows, "cols": cols, "size_px": [SW, SH], "rotation_deg": round(theta, 4),
                  "scale": round(scale, 5), "folds": folds},
        "instances": inst_truth,
        "defects": art_defects + print_defects,
        "expect_uncertain": expect_uncertain,
        "scan_artifacts": scan_artifacts,
        "variable_mask_mm": var_box,
        "sim_params": {**p, "mtf_sigma_px": mtf, "noise_sigma": noise, "emboss_strength": emb_strength,
                       "scanner": scanner_kind,
                       **({"scanner_model": {k: v for k, v in smodel.__dict__.items() if k != "params"}}
                          if smodel is not None else {})},
    }
    (out_dir / "truth.json").write_text(json.dumps(truth, indent=2, ensure_ascii=False), encoding="utf-8")

    job = {
        "schema": "printcmp.jobspec/1", "seed": 1,
        "reference": {"path": "reference.pdf"},
        "sample": {"path": "sample.tif", "kind": "scan"},
        "work_dpi": dpi,
        "instances": {"mode": "multi", "expected_layout": {"rows": rows, "cols": cols},
                      "rotations": [0, 180] if D["inst_rot180_p"] > 0 else [0]},
        "masks": ([{"reason": "variable", "rect_mm": var_box}] if var_box else []),
        "tolerance_profile": "label_standard",
        "outputs": {"dir": "out"},
    }
    (out_dir / "jobspec.json").write_text(json.dumps(job, indent=2, ensure_ascii=False), encoding="utf-8")
    return truth


def _line_hits_rect(p0, p1, rect):
    x0, y0, x1, y1 = rect
    # sampla linjen tätt (veck är långa raka linjer)
    for t in np.linspace(-2, 3, 2000):
        x = p0[0] + t * (p1[0] - p0[0])
        y = p0[1] + t * (p1[1] - p0[1])
        if x0 <= x <= x1 and y0 <= y <= y1:
            return True
    return False


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="cases")
    ap.add_argument("--n", type=int, default=4)
    ap.add_argument("--seed", type=int, default=100)
    ap.add_argument("--difficulty", default="normal", choices=list(DIFFICULTY))
    ap.add_argument("--template", default=None, choices=[None] + list(TEMPLATES))
    ap.add_argument("--rows", type=int, default=4)
    ap.add_argument("--cols", type=int, default=2)
    ap.add_argument("--mutations", default=None, help="kommaseparerad lista, t.ex. rule_delete,braille_char")
    ap.add_argument("--n-artwork", type=int, default=None)
    ap.add_argument("--scanner", default="legacy", choices=["legacy", "cis", "ccd"],
                    help="legacy = simulator v1; cis/ccd = flatbäddsmodell med fysiska veck")
    ap.add_argument("--fold-lift", type=float, default=None, help="fast vecklyft i mm (stresstest)")
    ap.add_argument("--fold-p", type=float, default=None, help="sannolikhet för veck (åsidosätter svårighet)")
    a = ap.parse_args(argv)
    out = Path(a.out)
    names = list(TEMPLATES)
    for i in range(a.n):
        seed = a.seed + i
        tpl = a.template or names[i % len(names)]
        d = out / f"case_{seed:05d}"
        t = make_case(tpl, seed, d, a.difficulty, a.cols, a.rows,
                      allowed=a.mutations.split(",") if a.mutations else None, n_artwork=a.n_artwork,
                      scanner_kind=a.scanner, fold_lift=a.fold_lift, fold_p=a.fold_p)
        print(f"{d.name}: {tpl}, {len(t['defects'])} fel "
              f"({sum(x['systematic'] for x in t['defects'])} systematiska), veck {len(t['sheet']['folds'])}, "
              f"rotation {t['sheet']['rotation_deg']:+.2f}°, scanner {a.scanner}"
              + "".join(f", lyft {f['lift_mm']} mm {f['kind']}" for f in t['sheet']['folds'] if 'lift_mm' in f),
              flush=True)


if __name__ == "__main__":
    main()
