"""Etikettbeskrivning (LabelSpec) -> PDF via reportlab, plus exakt geometri per element.

Koordinater i spec: mm från etikettens övre vänstra hörn (y nedåt), som referensramen.
Text: pos_mm = (x, baslinje-y).
"""
from __future__ import annotations

import io

from reportlab.graphics import renderPDF
from reportlab.graphics.barcode import eanbc, qr
from reportlab.graphics.shapes import Drawing
from reportlab.lib.colors import CMYKColor, CMYKColorSep
from reportlab.lib.units import mm
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.pdfgen import canvas

from printcmp import braille as brl

FONTS = {
    "DejaVuSans": "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "DejaVuSans-Bold": "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "DejaVuSerif": "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf",
    "DejaVuSansCondensed": "/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed.ttf",
}
_registered = set()


def ensure_font(name: str):
    if name not in _registered:
        pdfmetrics.registerFont(TTFont(name, FONTS[name]))
        _registered.add(name)


def _color(spec_color):
    c = spec_color or {"cmyk": [0, 0, 0, 1]}
    cmyk = c.get("cmyk", [0, 0, 0, 1])
    if c.get("spot"):
        return CMYKColorSep(*cmyk, spotName=c["spot"], density=1)
    return CMYKColor(*cmyk)


def text_metrics(el):
    """Returnerar (ascent_mm, descent_mm) (descent positiv nedåt)."""
    ensure_font(el["font"])
    face = pdfmetrics.getFont(el["font"]).face
    s = el["size_pt"] * 25.4 / 72
    return face.ascent / 1000 * s, -face.descent / 1000 * s


def text_width_mm(font, size_pt, s):
    ensure_font(font)
    return pdfmetrics.stringWidth(s, font, size_pt) * 25.4 / 72


def char_bbox_mm(el, i0, i1=None):
    """Ruta (x, y, w, h) i mm för tecken i0..i1 (exklusivt) i ett textelement."""
    i1 = i0 + 1 if i1 is None else i1
    t = el["text"]
    x0 = el["pos_mm"][0] + text_width_mm(el["font"], el["size_pt"], t[:i0])
    x1 = el["pos_mm"][0] + text_width_mm(el["font"], el["size_pt"], t[:i1])
    asc, desc = text_metrics(el)
    by = el["pos_mm"][1]
    return [x0, by - asc, max(x1 - x0, 0.05), asc + desc]


# ------------------------------------------------------------------ tabell
def table_layout(el):
    """Celler (textläge, rutor) och linjer för ett tabellelement. Allt i mm."""
    x, y = el["pos_mm"]
    cw, rh = el["col_widths_mm"], el["row_h_mm"]
    pad = el.get("pad_mm", 0.8)
    xs = [x]
    for w_ in cw:
        xs.append(xs[-1] + w_)
    dxs = el.get("row_dx_mm", {})
    cells = []
    for r, row in enumerate(el["rows"]):
        top = y + r * rh
        base = top + 0.70 * rh
        font = el.get("header_font", el["font"]) if (r == 0 and el.get("header", True)) else el["font"]
        for c, txt in enumerate(row):
            tw = text_width_mm(font, el["size_pt"], txt)
            off = float(dxs.get(str(r), 0.0))
            tx = (xs[c + 1] - pad - tw if el["align"][c] == "right" else xs[c] + pad) + off
            cells.append({"row": r, "col": c, "text": txt, "x": tx, "baseline": base, "font": font,
                          "size_pt": el["size_pt"], "cell": [xs[c], top, cw[c], rh], "width": tw})
    rules = []
    mode = el.get("rules", "grid")
    n = len(el["rows"])
    if mode in ("grid", "horizontal"):
        for i in range(n + 1):
            rules.append({"index": i, "orient": "h", "x0": xs[0], "y0": y + i * rh, "x1": xs[-1], "y1": y + i * rh})
    if mode == "grid":
        for j in range(len(cw) + 1):
            rules.append({"index": 100 + j, "orient": "v", "x0": xs[j], "y0": y, "x1": xs[j], "y1": y + n * rh})
    skip = set(el.get("skip_rules", []))
    rules = [r_ for r_ in rules if r_["index"] not in skip]
    return {"cells": cells, "rules": rules, "bbox": [xs[0], y, xs[-1] - xs[0], n * rh], "xs": xs}


def cell_text_bbox_mm(cell):
    asc, desc = text_metrics({"font": cell["font"], "size_pt": cell["size_pt"]})
    return [cell["x"], cell["baseline"] - asc, max(cell["width"], 0.05), asc + desc]


def rule_bbox_mm(rule, lw_mm):
    return [min(rule["x0"], rule["x1"]) - lw_mm / 2, min(rule["y0"], rule["y1"]) - lw_mm / 2,
            abs(rule["x1"] - rule["x0"]) + lw_mm, abs(rule["y1"] - rule["y0"]) + lw_mm]


# ------------------------------------------------------------------ punktskrift
def braille_dots_mm(el):
    """[(x, y, cell, dot)] i mm. origin_mm = mitten av punkt 1 i första cellen."""
    cells = brl.text_to_cells(el["text"])
    p = el.get("dot_pitch_mm", 2.5)
    c = el.get("cell_pitch_mm", 6.0)
    x0, y0 = el["origin_mm"]
    pts = []
    for i, dots in enumerate(cells):
        for n in sorted(dots):
            col = 0 if n <= 3 else 1
            row = (n - 1) % 3
            pts.append((x0 + i * c + col * p, y0 + row * p, i, n))
    return pts


def braille_cell_bbox_mm(el, i):
    p = el.get("dot_pitch_mm", 2.5)
    c = el.get("cell_pitch_mm", 6.0)
    r = el.get("dot_d_mm", 1.5) / 2
    x0, y0 = el["origin_mm"]
    return [x0 + i * c - r, y0 - r, p + 2 * r, 2 * p + 2 * r]


# ------------------------------------------------------------------ konturerad text
_ft_fonts = {}


def _ft(name):
    from fontTools.ttLib import TTFont as FTFont
    if name not in _ft_fonts:
        _ft_fonts[name] = FTFont(FONTS[name])
    return _ft_fonts[name]


def _draw_outlined(c, el, Y):
    from fontTools.pens.basePen import BasePen
    f = _ft(el["font"])
    upem = f["head"].unitsPerEm
    cmap = f.getBestCmap()
    gs = f.getGlyphSet()
    s = el["size_pt"] / upem
    x = el["pos_mm"][0] * mm
    yb = Y(el["pos_mm"][1])

    class Pen(BasePen):
        def __init__(self, path, ox):
            super().__init__(gs)
            self.p, self.ox = path, ox

        def _t(self, pt):
            return (self.ox + pt[0] * s, yb + pt[1] * s)

        def _moveTo(self, pt):
            self.p.moveTo(*self._t(pt))

        def _lineTo(self, pt):
            self.p.lineTo(*self._t(pt))

        def _curveToOne(self, p1, p2, p3):
            self.p.curveTo(*self._t(p1), *self._t(p2), *self._t(p3))

        def _closePath(self):
            self.p.close()

    c.setFillColor(_color(el.get("color")))
    for ch in el["text"]:
        g = cmap.get(ord(ch))
        if g is None:
            continue
        if ch != " ":
            path = c.beginPath()
            gs[g].draw(Pen(path, x))
            c.drawPath(path, stroke=0, fill=1)
        x += f["hmtx"][g][0] * s


def element_bbox_mm(el):
    t = el["type"]
    if t in ("text", "variable", "outlined"):
        return char_bbox_mm(el, 0, len(el["text"]))
    if t == "table":
        return list(table_layout(el)["bbox"])
    if t == "braille":
        n = len(brl.text_to_cells(el["text"]))
        a, b = braille_cell_bbox_mm(el, 0), braille_cell_bbox_mm(el, n - 1)
        return [a[0], a[1], b[0] + b[2] - a[0], a[3]]
    if t in ("rect", "ean13", "qr", "varnish"):
        return list(el["rect_mm"])
    if t == "circle":
        (cx, cy), r = el["center_mm"], el["r_mm"]
        return [cx - r, cy - r, 2 * r, 2 * r]
    if t == "poly":
        xs = [p[0] for p in el["points_mm"]]
        ys = [p[1] for p in el["points_mm"]]
        return [min(xs), min(ys), max(xs) - min(xs), max(ys) - min(ys)]
    raise ValueError(t)


def render_pdf(spec: dict) -> bytes:
    W, H = spec["size_mm"]
    buf = io.BytesIO()
    c = canvas.Canvas(buf, pagesize=(W * mm, H * mm))
    c.setTrimBox((0, 0, W * mm, H * mm))

    def Y(y_mm):
        return (H - y_mm) * mm

    layer_names: list[str] = []
    for el in spec["elements"]:
        t = el["type"]
        lay = el.get("layer")
        if lay:
            if lay not in layer_names:
                layer_names.append(lay)
            c.saveState()
            c._code.append(f"/OC /L{layer_names.index(lay)} BDC")
        _draw_element(c, el, t, Y)
        if lay:
            c._code.append("EMC")
            c.restoreState()

    dl = spec.get("dieline")
    if dl:
        c.setStrokeColor(CMYKColorSep(0, 1, 0, 0, spotName="CutContour", density=1))
        c.setLineWidth(0.25)
        r = dl.get("radius_mm", 0)
        c.roundRect(0, 0, W * mm, H * mm, r * mm, stroke=1, fill=0)
    c.showPage()
    c.save()
    pdf = buf.getvalue()
    return _add_ocgs(pdf, layer_names) if layer_names else pdf


def _add_ocgs(pdf_bytes: bytes, names: list[str]) -> bytes:
    """Gör /OC /Ln-markeringarna till riktiga PDF-lager (Optional Content Groups)."""
    import pikepdf
    pdf = pikepdf.open(io.BytesIO(pdf_bytes))
    ocgs = [pdf.make_indirect(pikepdf.Dictionary(Type=pikepdf.Name.OCG, Name=pikepdf.String(n))) for n in names]
    page = pdf.pages[0]
    res = page.Resources
    props = res.get("/Properties") or pikepdf.Dictionary()
    for i, g in enumerate(ocgs):
        props[f"/L{i}"] = g
    res.Properties = props
    pdf.Root.OCProperties = pikepdf.Dictionary(
        OCGs=pikepdf.Array(ocgs),
        D=pikepdf.Dictionary(Order=pikepdf.Array(ocgs), ON=pikepdf.Array(ocgs), OFF=pikepdf.Array()))
    out = io.BytesIO()
    pdf.save(out)
    return out.getvalue()


def _draw_element(c, el, t, Y):
    if True:
        if t in ("text", "variable"):
            ensure_font(el["font"])
            c.setFillColor(_color(el.get("color")))
            c.setFont(el["font"], el["size_pt"])
            c.drawString(el["pos_mm"][0] * mm, Y(el["pos_mm"][1]), el["text"])
        elif t == "outlined":
            _draw_outlined(c, el, Y)
        elif t == "table":
            lay = table_layout(el)
            c.setFillColor(_color(el.get("color")))
            for cell in lay["cells"]:
                ensure_font(cell["font"])
                c.setFont(cell["font"], cell["size_pt"])
                c.drawString(cell["x"] * mm, Y(cell["baseline"]), cell["text"])
            c.setStrokeColor(_color(el.get("rule_color")))
            c.setLineWidth(el.get("rule_w_mm", 0.15) * mm)
            c.setLineCap(2)
            for r_ in lay["rules"]:
                c.line(r_["x0"] * mm, Y(r_["y0"]), r_["x1"] * mm, Y(r_["y1"]))
        elif t == "braille":
            # prägling: egen dekorfärg som inte trycks
            c.setFillColor(CMYKColorSep(0, 0.6, 0, 0, spotName=el.get("spot", "Braille"), density=1))
            rr = el.get("dot_d_mm", 1.5) / 2
            for x_, y_, _, _ in braille_dots_mm(el):
                c.circle(x_ * mm, Y(y_), rr * mm, stroke=0, fill=1)
        elif t == "rect":
            x, y, w, h = el["rect_mm"]
            c.setFillColor(_color(el.get("color")))
            c.rect(x * mm, Y(y + h), w * mm, h * mm, stroke=0, fill=1)
        elif t == "circle":
            (cx, cy), r = el["center_mm"], el["r_mm"]
            c.setFillColor(_color(el.get("color")))
            c.circle(cx * mm, Y(cy), r * mm, stroke=0, fill=1)
        elif t == "poly":
            c.setFillColor(_color(el.get("color")))
            p = c.beginPath()
            pts = el["points_mm"]
            p.moveTo(pts[0][0] * mm, Y(pts[0][1]))
            for x, y in pts[1:]:
                p.lineTo(x * mm, Y(y))
            p.close()
            c.drawPath(p, stroke=0, fill=1)
        elif t == "varnish":
            x, y, w, h = el["rect_mm"]
            c.setFillColor(CMYKColorSep(0, 0, 0, 0.15, spotName="Varnish", density=1))
            c.rect(x * mm, Y(y + h), w * mm, h * mm, stroke=0, fill=1)
        elif t in ("ean13", "qr"):
            x, y, w, h = el["rect_mm"]
            if t == "ean13":
                wd = eanbc.Ean13BarcodeWidget(el["data"][:12], fontSize=8, barHeight=36, humanReadable=True)
            else:
                wd = qr.QrCodeWidget(el["data"], barLevel="M")
            b = wd.getBounds()
            bw, bh = b[2] - b[0], b[3] - b[1]
            sx, sy = (w * mm) / bw, (h * mm) / bh
            if t == "qr":
                sx = sy = min(sx, sy)
            d = Drawing(w * mm, h * mm, transform=[sx, 0, 0, sy, -b[0] * sx, -b[1] * sy])
            d.add(wd)
            renderPDF.draw(d, c, x * mm, Y(y + h))
        else:
            raise ValueError(f"okänd elementtyp {t}")
