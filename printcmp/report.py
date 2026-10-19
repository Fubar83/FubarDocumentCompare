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
           "Shape": "Form", "Barcode": "Streckkod", "Uncertain": "Osäker zon", "Braille": "Punktskrift"}
CAT_SV = {"Text": "Text", "OutlinedText": "Konturerad text", "TableCell": "Tabellcell", "TableRule": "Tabellinje",
          "Barcode": "Streckkod", "Braille": "Punktskrift", "Image": "Bild", "Vector": "Grafik", "Rule": "Linje",
          "Background": "Bakgrund", "TextLayer": "Textlager"}
STATUS_SV = {"pass": "Godkänd", "warn": "Varning", "fail": "Fel"}
LEVEL_SV = {"confirmed": ("Bekräftade avvikelser", "Oberoende belägg: avkodat innehåll skiljer, eller samma tydliga "
                          "avvikelse på samma ställe i (nästan) alla etiketter."),
            "high": ("Mycket sannolika", "Tydlig signal långt över toleransen, utanför veck."),
            "probable": ("Troliga", "Signal över toleransen men nära den, eller i ett område med sämre underlag."),
            "possible": ("Möjliga – kontrollera manuellt", "Osäkra zoner (veck, registrering), svag signal "
                         "eller färg uppmätt med okalibrerad scanner.")}
LEVEL_COLOR = {"confirmed": "#be123c", "high": "#dc2626", "probable": "#d97706", "possible": "#64748b"}
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


def _contents(result) -> str:
    ref = result["reference"]
    rows = []
    for t in ref.get("tables", []):
        kind = {"ruled_grid": "med rutnät", "ruled_horizontal": "med vågräta linjer", "unruled": "utan linjer"}[t["kind"]]
        first = " · ".join(html.escape(c) for c in t["cells"][0] if c) if t["cells"] else ""
        rows.append(f"<li>Tabell {t['id']} ({kind}): {t['rows']} rader × {t['cols']} kolumner"
                    f"{', ' + str(t['rules']) + ' linjer' if t['rules'] else ''}"
                    f" – <span class=muted>{first}</span></li>")
    for b in ref.get("braille", []):
        rows.append(f"<li>Punktskrift: \u201d{html.escape(b['text'])}\u201d {b['unicode']} "
                    f"<span class=muted>({b['cells']} celler, punktavstånd {b['dot_pitch_mm']} mm, "
                    f"cellavstånd {b['cell_pitch_mm']} mm)</span></li>")
    if ref.get("outlined_text_mm"):
        rows.append(f"<li>Konturerad text: {len(ref['outlined_text_mm'])} rad(er)</li>")
    for c in ref.get("barcodes", []):
        rows.append(f"<li>Streckkod {html.escape(c['symbology'])}: {html.escape(c['data'])}</li>")
    lay = ref.get("layers") or {}
    if lay.get("ocgs"):
        comp = lay.get("compared") or {}
        items = []
        for g in lay["ocgs"]:
            on = comp.get(g["name"], True)
            items.append(f'<span class="layer {"on" if on else "off"}">{"✓" if on else "✗"} {html.escape(g["name"])}</span>')
        rows.append("<li>PDF-lager (✓ jämförs, ✗ uteslutet): " + " ".join(items) + "</li>")
    if lay.get("spots"):
        rows.append("<li>Dekorfärger: " + ", ".join(html.escape(x) for x in lay["spots"]) + "</li>")
    if not rows:
        return ""
    return "<h2>Identifierat i originalet</h2><ul>" + "".join(rows) + "</ul>"


def _level_section(level: str, rows: list[str]) -> str:
    if not rows:
        return ""
    title, desc = LEVEL_SV[level]
    return (f'<h3 id="lv-{level}" style="--c:{LEVEL_COLOR[level]}">{title} ({len(rows)})</h3>'
            f'<p class="muted lvdesc">{desc}</p>'
            '<table><thead><tr><th>#</th><th>Säkerhet</th><th>Allvar</th><th>Typ</th><th>Instanser</th><th>Position</th>'
            '<th>Detaljer</th><th>Utsnitt</th></tr></thead><tbody>' + "".join(rows) + '</tbody></table>')


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

    rows_by_level = {k: [] for k in LEVEL_SV}
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
        if d.get("category") in CAT_SV:
            details.insert(0, f"<b>{CAT_SV[d['category']]}</b>")
        ti = d.get("table")
        if ti:
            if ti.get("element") == "rule":
                if ti.get("between_rows"):
                    details.append(f"Tabell {ti['table']}: linjen mellan rad {ti['between_rows'][0] + 1} och "
                                   f"{ti['between_rows'][1] + 1}")
                else:
                    details.append(f"Tabell {ti['table']}: {'yttre ram' if ti.get('orient') == 'h' else 'lodrät linje'}")
            elif ti.get("row_shift"):
                details.append(f"Tabell {ti['table']}: rad {ti['row'] + 1} förskjuten "
                               f"(\u201d{html.escape(ti.get('row_label') or '')}\u201d)")
            else:
                where_ = f"rad {ti['row'] + 1}" + (f", kolumn {ti['col'] + 1}" if ti.get("col") is not None else "")
                lab = html.escape(ti.get("row_label") or "")
                cell = html.escape(ti.get("cell_text") or "")
                details.append(f"Tabell {ti['table']}: {where_}" + (f" – {lab}" if lab else "") +
                               (f" = \u201d{cell}\u201d i originalet" if cell and ti.get("col") else ""))
        bi = d.get("braille")
        if bi and d["kind"] == "Braille":
            if d.get("cause") == "embossing_missing":
                details.append(f"Prägling saknas för hela raden (väntat \u201d{html.escape(bi.get('expected_text', ''))}\u201d)")
            elif "cell" in bi:
                details.append(f"Cell {bi['cell'] + 1}: väntat \u201d{html.escape(bi['expected'])}\u201d "
                               f"{bi['expected_unicode']}, läst \u201d{html.escape(bi['read'])}\u201d {bi['read_unicode']}")
                if bi.get("missing_dots"):
                    details.append("Saknade punkter: " + ", ".join(map(str, bi["missing_dots"])))
                if bi.get("extra_dots"):
                    details.append("Extra punkter: " + ", ".join(map(str, bi["extra_dots"])))
        for tc in (d.get("text_change") or [])[:4]:
            kind_sv = {"Changed": "ändrad", "Missing": "borttagen", "Added": "tillagd", "Moved": "flyttad",
                       "Style": "typsnitt/storlek"}.get(tc.get("kind"), "")
            if tc.get("kind") == "Style":
                details.append(f"Text \u201d{html.escape(tc['from'])}\u201d: {html.escape(tc.get('font_from') or '')} "
                               f"{tc.get('size_from')} pt → {html.escape(tc.get('font_to') or '')} {tc.get('size_to')} pt")
            elif tc.get("kind") == "Moved":
                details.append(f"Text \u201d{html.escape(tc['from'])}\u201d flyttad {tc.get('shift_mm')} mm")
            else:
                diff_ = f' <span class=muted>({html.escape(tc["diff"])})</span>' if tc.get("diff") else ""
                details.append(f"Text {kind_sv}: \u201d<del>{html.escape(tc.get('from', ''))}</del>\u201d → "
                               f"\u201d<ins>{html.escape(tc.get('to', ''))}</ins>\u201d{diff_}")
        if d.get("text_only"):
            details.append("<i>Endast i textlagret – syns inte i tryck</i>")
        if d.get("barcode"):
            bcd = d["barcode"]
            details.append(f"Streckkod: väntat {html.escape(str(bcd.get('expected')))}, läst {html.escape(str(bcd.get('read')))}")
        if d.get("cause"):
            details.append({"fold": "Veck", "registration": "Osäker registrering"}.get(d["cause"], d["cause"]))
        if d.get("source") == "consensus":
            details.append("Hittad via jämförelse mellan instanser")
        cert = d.get("certainty") or {"level": "possible", "score": 0, "reasons": []}
        lvl_col = LEVEL_COLOR[cert["level"]]
        cert_html = (f'<div class=cert><div class=bar><i style="width:{cert["score"]}%;background:{lvl_col}"></i></div>'
                     f'<b>{cert["score"]} %</b></div><ul class=why>'
                     + "".join(f"<li>{html.escape(r)}</li>" for r in cert["reasons"]) + "</ul>")
        rows_by_level[cert["level"]].append(
            f'<tr><td class=num>{d["id"]}</td>'
            f'<td>{cert_html}</td>'
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
            f'{" · veck" if io["folds"] else ""}'
            f'{(" · punktskrift: " + html.escape(" / ".join(r or "–" for r in io["braille"]["read"]))) if io.get("braille") else ""}'
            f'</span></figcaption></figure>')

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
.pair{{display:flex;gap:8px;flex-wrap:wrap}}
.layer{{display:inline-block;padding:1px 8px;margin:2px;border-radius:99px;border:1px solid var(--line);font-size:12px}}
.layer.off{{color:var(--muted);text-decoration:line-through}} del{{color:var(--major)}} ins{{color:var(--pass);text-decoration:none;font-weight:600}}
.levels{{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:10px;margin:8px 0 16px}}
.lv{{display:block;text-decoration:none;color:var(--fg);background:var(--card);border:1px solid var(--line);border-left:5px solid var(--c);border-radius:10px;padding:10px 12px}}
.lv b{{display:block;font-size:22px;color:var(--c)}}
h3{{font-size:15px;margin:28px 0 4px;padding-left:10px;border-left:5px solid var(--c)}} .lvdesc{{margin:0 0 10px 15px}}
.cert{{display:flex;align-items:center;gap:8px;min-width:120px}} .bar{{flex:1;height:8px;background:var(--line);border-radius:99px;overflow:hidden}}
.bar i{{display:block;height:100%}} ul.why{{margin:6px 0 0;padding-left:16px;font-size:12px;color:var(--muted);max-width:260px}} figure{{margin:0}} figcaption{{font-size:12px;color:var(--muted);margin-top:4px}}
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
<div class=card><b>{(sh["rotation_deg"] + 90) % 180 - 90:+.2f}°</b>arkets snedställning</div>
</div>
{_contents(result)}
<h2>Ark</h2><img src="{imageio.png_data_uri(sheet)}" alt="Arköversikt">
<h2>Skillnader, säkrast först</h2>
<div class=levels>{"".join(f'<a class=lv href="#lv-{k}" style="--c:{LEVEL_COLOR[k]}"><b>{len(v)}</b>{LEVEL_SV[k][0]}</a>' for k, v in rows_by_level.items())}</div>
{"".join(_level_section(k, v) for k, v in rows_by_level.items()) or '<p>Inga skillnader hittades.</p>'}
<h2>Instanser</h2><div class=gallery>{"".join(gallery)}</div>
<p class=muted>Tider (ms): {", ".join(f"{k} {v}" for k, v in result["timing_ms"].items())}</p>
</main></body></html>"""
    Path(path).write_text(doc, encoding="utf-8")
