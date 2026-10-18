"""Fristående HTML-rapport med inbäddade PNG-miniatyrer (data-URI)."""
from __future__ import annotations

import html
from pathlib import Path

import cv2
import numpy as np

from . import imageio
from .pipeline import KIND_COLORS

SEV_CLASS = {"Critical": "crit", "Major": "major", "Minor": "minor", "Info": "info"}
KIND_SV = {"Missing": "Saknas", "Added": "Tillagt", "Changed": "Ändrat", "Moved": "Flyttat", "Color": "Färg",
           "Shape": "Form", "Barcode": "Streckkod", "Uncertain": "Osäker zon"}
STATUS_SV = {"pass": "Godkänd", "warn": "Varning", "fail": "Fel"}
STATUS_RGB = {"pass": (22, 163, 74), "warn": (217, 119, 6), "fail": (220, 38, 38)}


def _thumb(img, max_w, max_h=220):
    h, w = img.shape[:2]
    s = min(1.0, max_w / w, max_h / h)
    if s >= 1.0:
        return img
    return cv2.resize(img, (max(1, int(w * s)), max(1, int(h * s))), interpolation=cv2.INTER_AREA)


def _crop(img, bbox_mm, dpi, pad_mm=2.0, min_mm=6.0):
    ppm = dpi / 25.4
    x, y, w, h = bbox_mm
    cx, cy = x + w / 2, y + h / 2
    hw, hh = max(w / 2 + pad_mm, min_mm / 2), max(h / 2 + pad_mm, min_mm / 2)
    H, W = img.shape[:2]
    x0, y0 = int(max(0, (cx - hw) * ppm)), int(max(0, (cy - hh) * ppm))
    x1, y1 = int(min(W, (cx + hw) * ppm)), int(min(H, (cy + hh) * ppm))
    c = img[y0:y1, x0:x1].copy()
    bx0, by0 = int(x * ppm) - x0, int(y * ppm) - y0
    bx1, by1 = int((x + w) * ppm) - x0, int((y + h) * ppm) - y0
    return c, (bx0, by0, bx1, by1)


def write_html(path: Path, result: dict, ref_rgb, sample_rgb, insts, dpi, sample_dpi, layers_dir: Path | None):
    # --- arköversikt ---
    max_w = 900
    s = min(1.0, max_w / sample_rgb.shape[1])
    sheet = cv2.resize(sample_rgb, None, fx=s, fy=s, interpolation=cv2.INTER_AREA)
    h, w = ref_rgb.shape[:2]
    corners = np.array([[0, 0, 1], [w, 0, 1], [w, h, 1], [0, h, 1]], np.float64)
    for io in result["instances"]:
        H = np.array(io["transform"]["h"]).reshape(3, 3)
        p = corners @ H.T
        p = p[:, :2] / p[:, 2:3] * s
        col = STATUS_RGB[io["status"]]
        cv2.polylines(sheet, [np.round(p).astype(np.int32)], True, col, 2, cv2.LINE_AA)
        c = p.mean(axis=0)
        cv2.putText(sheet, str(io["index"]), (int(c[0]) - 8, int(c[1]) + 8), cv2.FONT_HERSHEY_SIMPLEX, 0.8, col, 2, cv2.LINE_AA)

    layer_dpi = None
    layers = {}
    if layers_dir is not None:
        for io in result["instances"]:
            i = io["index"]
            rp, sp, op = (layers_dir / f"inst{i:03d}_{k}.png" for k in ("ref", "sample", "overlay"))
            if rp.exists():
                layers[i] = tuple(imageio.read_scan(p, 1)[0] for p in (rp, sp, op))
        if layers:
            any_ref = next(iter(layers.values()))[0]
            layer_dpi = dpi * any_ref.shape[1] / ref_rgb.shape[1]

    rows = []
    for d in result["differences"]:
        thumbs = ""
        if d.get("bbox_mm") and layers and d["instances"] and d["kind"] != "Uncertain":
            inst = d["instances"][0]
            if inst in layers:
                r_img, s_img, _ = layers[inst]
                rc, b = _crop(r_img, d["bbox_mm"], layer_dpi)
                sc, _ = _crop(s_img, d["bbox_mm"], layer_dpi)
                col = KIND_COLORS.get(d["kind"], (0, 0, 0))
                for im in (rc, sc):
                    cv2.rectangle(im, (b[0] - 2, b[1] - 2), (b[2] + 2, b[3] + 2), col, 1, cv2.LINE_AA)
                thumbs = (f'<div class="pair"><figure><img src="{imageio.png_data_uri(_thumb(rc, 260))}"><figcaption>Referens</figcaption></figure>'
                          f'<figure><img src="{imageio.png_data_uri(_thumb(sc, 260))}"><figcaption>Prov (instans {inst})</figcaption></figure></div>')
        where = "–"
        if d.get("bbox_mm"):
            x, y, bw, bh = d["bbox_mm"]
            where = f"x {x:.1f}, y {y:.1f} mm<br><span class=muted>{bw:.1f} × {bh:.1f} mm</span>"
        insts_txt = ("Systematisk: " if d.get("systematic") else "") + (", ".join(map(str, d["instances"])) or "–")
        if d.get("reason") == "instance_not_found":
            insts_txt = f"Hittade {d['found']} av {d['expected']} instanser"
        details = []
        texts = [o["text"] for o in d.get("objects", []) if o.get("text")]
        if texts:
            details.append("Text: " + html.escape(" ".join(texts[:6])))
        m = d.get("metrics") or {}
        if m.get("shift_mm"):
            details.append(f"Förskjutning {m['shift_mm']:.2f} mm")
        if m.get("delta_e") is not None and d["kind"] == "Color":
            details.append(f"ΔE00 {m['delta_e']:.1f}")
        if d.get("barcode"):
            bcd = d["barcode"]
            details.append(f"Streckkod: väntat {html.escape(str(bcd.get('expected')))}, läst {html.escape(str(bcd.get('read')))}")
        if d.get("cause"):
            details.append({"fold": "Veck", "registration": "Osäker registrering"}.get(d["cause"], d["cause"]))
        if d.get("source") == "consensus":
            details.append("Hittad via jämförelse mellan instanser")
        rows.append(
            f'<tr><td class=num>{d["id"]}</td>'
            f'<td><span class="sev {SEV_CLASS[d["severity"]]}">{d["severity"]}</span></td>'
            f'<td><span class=kind style="--k: rgb{KIND_COLORS.get(d["kind"], (0, 0, 0))}">{KIND_SV.get(d["kind"], d["kind"])}</span></td>'
            f'<td>{insts_txt}</td><td>{where}</td><td>{"<br>".join(details) or "–"}</td><td>{thumbs}</td></tr>')

    gallery = []
    for io in result["instances"]:
        i = io["index"]
        img = ""
        if i in layers:
            img = f'<img src="{imageio.png_data_uri(_thumb(layers[i][2], 360, 400))}">'
        rms = io["registration_rms_px"]
        gallery.append(
            f'<figure class="inst {io["status"]}">{img}<figcaption><b>Instans {i}</b> · rad {io["row"] + 1}, kol {io["col"] + 1}'
            f' · <span class="st {io["status"]}">{STATUS_SV[io["status"]]}</span><br>'
            f'<span class=muted>registrering {("%.2f px" % rms) if rms is not None else "–"} · ΔE medel '
            f'{io["mean_delta_e"] if io["mean_delta_e"] is not None else "–"}'
            f'{" · veck" if io["folds"] else ""}</span></figcaption></figure>')

    sm = result["summary"]
    sh = result["sheet"]
    doc = f"""<!doctype html>
<html lang="sv"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Jämförelserapport</title>
<style>
:root{{--bg:#fafaf9;--fg:#1c1917;--muted:#78716c;--card:#fff;--line:#e7e5e4;--crit:#be123c;--major:#dc2626;--minor:#d97706;--info:#64748b;--pass:#16a34a}}
@media (prefers-color-scheme:dark){{:root{{--bg:#0c0a09;--fg:#e7e5e4;--muted:#a8a29e;--card:#1c1917;--line:#292524}}}}
*{{box-sizing:border-box}} body{{margin:0;background:var(--bg);color:var(--fg);font:14px/1.45 system-ui,-apple-system,Segoe UI,sans-serif}}
main{{max-width:1200px;margin:0 auto;padding:24px 16px 64px}} h1{{font-size:22px;margin:0 0 4px}} h2{{font-size:16px;margin:32px 0 12px}}
.muted{{color:var(--muted)}} .cards{{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:12px;margin:16px 0}}
.card{{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px}} .card b{{display:block;font-size:22px}}
img{{max-width:100%;display:block;border-radius:6px;border:1px solid var(--line)}}
table{{width:100%;border-collapse:collapse;background:var(--card);border:1px solid var(--line);border-radius:10px;overflow:hidden}}
th,td{{text-align:left;vertical-align:top;padding:10px;border-bottom:1px solid var(--line)}} th{{font-size:12px;color:var(--muted);font-weight:600}}
td.num{{color:var(--muted)}} .sev{{font-size:12px;font-weight:600;padding:2px 8px;border-radius:99px;color:#fff}}
.sev.crit{{background:var(--crit)}} .sev.major{{background:var(--major)}} .sev.minor{{background:var(--minor)}} .sev.info{{background:var(--info)}}
.kind{{border-left:4px solid var(--k);padding-left:6px;font-weight:600}}
.pair{{display:flex;gap:8px;flex-wrap:wrap}} figure{{margin:0}} figcaption{{font-size:12px;color:var(--muted);margin-top:4px}}
.gallery{{display:grid;grid-template-columns:repeat(auto-fill,minmax(260px,1fr));gap:16px}}
.inst{{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:10px}}
.st.pass{{color:var(--pass)}} .st.warn{{color:var(--minor)}} .st.fail{{color:var(--major)}}
@media (max-width:760px){{table,thead,tbody,tr,td,th{{display:block}} thead{{display:none}} td{{border:0;padding:4px 10px}} tr{{border-bottom:1px solid var(--line);padding:8px 0}}}}
</style></head><body><main>
<h1>Jämförelserapport</h1>
<div class=muted>Referens: {html.escape(Path(result["reference"]["path"]).name)} · Prov: {html.escape(Path(result["sample"]["path"]).name)} · printcmp {result["engine"]["printcmp"]}</div>
<div class=cards>
<div class=card><b>{sh["instances_found"]}{" / " + str(sh["instances_expected"]) if sh["instances_expected"] else ""}</b>instanser hittade</div>
<div class=card><b style="color:var(--major)">{sm["instances_fail"]}</b>underkända</div>
<div class=card><b style="color:var(--minor)">{sm["instances_warn"]}</b>med varning</div>
<div class=card><b style="color:var(--pass)">{sm["instances_pass"]}</b>godkända</div>
<div class=card><b>{sm["differences"]}</b>skillnader ({sm["systematic"]} systematiska)</div>
<div class=card><b>{sh["rotation_deg"]:+.2f}°</b>arkets rotation</div>
</div>
<h2>Ark</h2><img src="{imageio.png_data_uri(sheet)}" alt="Arköversikt">
<h2>Skillnader</h2>
<table><thead><tr><th>#</th><th>Allvar</th><th>Typ</th><th>Instanser</th><th>Position</th><th>Detaljer</th><th>Utsnitt</th></tr></thead>
<tbody>{"".join(rows) or '<tr><td colspan=7>Inga skillnader hittades.</td></tr>'}</tbody></table>
<h2>Instanser</h2><div class=gallery>{"".join(gallery)}</div>
<p class=muted>Tider (ms): {", ".join(f"{k} {v}" for k, v in result["timing_ms"].items())}</p>
</main></body></html>"""
    Path(path).write_text(doc, encoding="utf-8")
