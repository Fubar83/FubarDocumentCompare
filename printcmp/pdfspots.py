"""Dekorfärger som inte trycks (stanslinje, lack, folie).

Tolkar sidans innehållsström med en minimal operatortolk (pikepdf):
- samlar banor som målas i en Separation-färg vars namn matchar en regel
  (t.ex. "CutContour" -> stanslinje, "Varnish" -> lack), i PDF-koordinater,
- skapar en "sanerad" PDF där dessa målningar tagits bort, så att
  referensrenderingen motsvarar det som faktiskt trycks.

Motsvarar modulen pdf/dieline i den planerade C++-kärnan.
"""
from __future__ import annotations

import io
import math
import re
from dataclasses import dataclass, field

import pikepdf

DEFAULT_RULES = {
    "dieline": r"(?i)cut|die|stans|kiss|crease|perf",
    "varnish": r"(?i)varnish|lack|foil|folie|hot ?stamp|emboss",
}

IDENTITY = (1.0, 0.0, 0.0, 1.0, 0.0, 0.0)


def _mul(m1, m2):
    """Matris m1 följd av m2 (PDF-konvention, radvektorer)."""
    a1, b1, c1, d1, e1, f1 = m1
    a2, b2, c2, d2, e2, f2 = m2
    return (
        a1 * a2 + b1 * c2,
        a1 * b2 + b1 * d2,
        c1 * a2 + d1 * c2,
        c1 * b2 + d1 * d2,
        e1 * a2 + f1 * c2 + e2,
        e1 * b2 + f1 * d2 + f2,
    )


def _apply(m, x, y):
    a, b, c, d, e, f = m
    return (a * x + c * y + e, b * x + d * y + f)


@dataclass
class SpotPath:
    role: str            # "dieline" | "varnish"
    spot_name: str
    paint: str           # "stroke" | "fill"
    polygons: list       # list[list[(x, y)]] i PDF-punkter (sidkoordinater)


@dataclass
class SpotAnalysis:
    paths: list = field(default_factory=list)
    spot_names: list = field(default_factory=list)   # alla Separation-namn på sidan
    sanitized_pdf: bytes = b""

    def polygons(self, role: str):
        out = []
        for p in self.paths:
            if p.role == role:
                out.extend(p.polygons)
        return out


def _bezier(p0, p1, p2, p3, n=12):
    pts = []
    for i in range(1, n + 1):
        t = i / n
        mt = 1 - t
        x = mt**3 * p0[0] + 3 * mt**2 * t * p1[0] + 3 * mt * t**2 * p2[0] + t**3 * p3[0]
        y = mt**3 * p0[1] + 3 * mt**2 * t * p1[1] + 3 * mt * t**2 * p2[1] + t**3 * p3[1]
        pts.append((x, y))
    return pts


def _colorspace_names(resources) -> dict:
    """Resursnamn -> Separation-namn (eller None)."""
    out = {}
    if resources is None or "/ColorSpace" not in resources:
        return out
    for key, val in resources.ColorSpace.items():
        try:
            if isinstance(val, pikepdf.Array) and len(val) >= 2 and str(val[0]) == "/Separation":
                out[str(key)] = str(val[1]).lstrip("/")
            elif isinstance(val, pikepdf.Array) and len(val) >= 2 and str(val[0]) == "/DeviceN":
                names = [str(n).lstrip("/") for n in val[1]]
                out[str(key)] = "+".join(names)
        except Exception:  # noqa: BLE001 - robust mot ovanliga resurser
            continue
    return out


def analyze(pdf_bytes: bytes, page_index: int = 0, rules: dict | None = None) -> SpotAnalysis:
    rules = rules or DEFAULT_RULES
    compiled = {role: re.compile(rx) for role, rx in rules.items()}

    def role_of(spot):
        if spot is None:
            return None
        for role, rx in compiled.items():
            if rx.search(spot):
                return role
        return None

    pdf = pikepdf.open(io.BytesIO(pdf_bytes))
    page = pdf.pages[page_index]
    cs_map = _colorspace_names(page.Resources if "/Resources" in page else None)

    result = SpotAnalysis(spot_names=sorted(set(v for v in cs_map.values() if v)))
    ops = pikepdf.parse_content_stream(page)
    new_ops = []

    ctm = IDENTITY
    stack = []
    stroke_spot = None
    fill_spot = None
    subpaths: list[list] = []
    cur: list = []
    start = None

    def close_cur():
        nonlocal cur
        if cur:
            subpaths.append(cur)
        cur = []

    for operands, op in ops:
        o = str(op)
        keep = True
        if o == "q":
            stack.append((ctm, stroke_spot, fill_spot))
        elif o == "Q":
            if stack:
                ctm, stroke_spot, fill_spot = stack.pop()
        elif o == "cm":
            m = tuple(float(v) for v in operands)
            ctm = _mul(m, ctm)
        elif o == "CS":
            stroke_spot = cs_map.get(str(operands[0]))
        elif o == "cs":
            fill_spot = cs_map.get(str(operands[0]))
        elif o in ("G", "RG", "K"):
            stroke_spot = None
        elif o in ("g", "rg", "k"):
            fill_spot = None
        elif o == "m":
            close_cur()
            x, y = float(operands[0]), float(operands[1])
            start = _apply(ctm, x, y)
            cur = [start]
        elif o == "l":
            cur.append(_apply(ctm, float(operands[0]), float(operands[1])))
        elif o in ("c", "v", "y"):
            vals = [float(v) for v in operands]
            p0 = cur[-1] if cur else (0.0, 0.0)
            if o == "c":
                p1 = _apply(ctm, vals[0], vals[1]); p2 = _apply(ctm, vals[2], vals[3]); p3 = _apply(ctm, vals[4], vals[5])
            elif o == "v":
                p1 = p0; p2 = _apply(ctm, vals[0], vals[1]); p3 = _apply(ctm, vals[2], vals[3])
            else:
                p1 = _apply(ctm, vals[0], vals[1]); p2 = _apply(ctm, vals[2], vals[3]); p3 = p2
            cur.extend(_bezier(p0, p1, p2, p3))
        elif o == "re":
            close_cur()
            x, y, w, h = (float(v) for v in operands)
            subpaths.append([_apply(ctm, x, y), _apply(ctm, x + w, y), _apply(ctm, x + w, y + h), _apply(ctm, x, y + h)])
        elif o == "h":
            if cur and start is not None:
                cur.append(cur[0])
        elif o in ("S", "s", "f", "F", "f*", "B", "B*", "b", "b*", "n"):
            close_cur()
            strokes = o in ("S", "s", "B", "B*", "b", "b*")
            fills = o in ("f", "F", "f*", "B", "B*", "b", "b*")
            s_role = role_of(stroke_spot) if strokes else None
            f_role = role_of(fill_spot) if fills else None
            polys = [p for p in subpaths if len(p) >= 2]
            if s_role and polys:
                result.paths.append(SpotPath(s_role, stroke_spot, "stroke", polys))
            if f_role and polys:
                result.paths.append(SpotPath(f_role, fill_spot, "fill", polys))
            # Sanering: ta bort målningen i icke-tryckande färger
            if o != "n" and (s_role or f_role):
                stroke_keep = strokes and not s_role
                fill_keep = fills and not f_role
                even_odd = o in ("f*", "B*", "b*")
                if stroke_keep and fill_keep:
                    repl = o
                elif fill_keep:
                    repl = "f*" if even_odd else "f"
                elif stroke_keep:
                    repl = "S"
                else:
                    repl = "n"
                new_ops.append(pikepdf.ContentStreamInstruction([], pikepdf.Operator(repl)))
                keep = False
            subpaths = []
            start = None
        if keep:
            new_ops.append(pikepdf.ContentStreamInstruction(operands, op))

    page.Contents = pdf.make_stream(pikepdf.unparse_content_stream(new_ops))
    buf = io.BytesIO()
    pdf.save(buf)
    result.sanitized_pdf = buf.getvalue()
    return result


def polygon_area(poly) -> float:
    a = 0.0
    for (x1, y1), (x2, y2) in zip(poly, poly[1:] + poly[:1]):
        a += x1 * y2 - x2 * y1
    return abs(a) / 2


def polygon_length(poly) -> float:
    return sum(math.dist(p, q) for p, q in zip(poly, poly[1:]))
