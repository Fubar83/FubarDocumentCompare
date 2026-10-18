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


def element_bbox_mm(el):
    t = el["type"]
    if t in ("text", "variable"):
        return char_bbox_mm(el, 0, len(el["text"]))
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

    for el in spec["elements"]:
        t = el["type"]
        if t in ("text", "variable"):
            ensure_font(el["font"])
            c.setFillColor(_color(el.get("color")))
            c.setFont(el["font"], el["size_pt"])
            c.drawString(el["pos_mm"][0] * mm, Y(el["pos_mm"][1]), el["text"])
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

    dl = spec.get("dieline")
    if dl:
        c.setStrokeColor(CMYKColorSep(0, 1, 0, 0, spotName="CutContour", density=1))
        c.setLineWidth(0.25)
        r = dl.get("radius_mm", 0)
        c.roundRect(0, 0, W * mm, H * mm, r * mm, stroke=1, fill=0)
    c.showPage()
    c.save()
    return buf.getvalue()
