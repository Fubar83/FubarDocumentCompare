"""PDF-lager: lista och välj vilka lager som ska ingå i jämförelsen.

Två sorters "lager" finns i tryckfiler:

* PDF-lager (Optional Content Groups, OCG): t.ex. "Text", "Bild", "Stans", "Lack",
  språkversioner. Innehållet märks i innehållsströmmen med ``/OC /namn BDC … EMC``
  eller genom att ett Form-XObject har ``/OC``.
* Dekorfärger (Separation): t.ex. "CutContour", "Varnish", "White". Hanteras av
  ``pdfspots`` (stanslinje/lack/punktskrift) och kan här dessutom uteslutas på namn.

``filter_pdf`` tar bort uteslutna lager ur innehållsströmmarna (inte bara döljer dem),
så att både rendering och textutvinning ser samma innehåll. Lagerstatus i
``/OCProperties`` sätts också, så att en visare visar samma sak.

Regler: ``include`` (endast dessa lager) och ``exclude`` (alla utom dessa) är listor
med namn eller reguljära uttryck (skiftlägesokänsliga). Innehåll utan lager påverkas
aldrig av ``include``.
"""
from __future__ import annotations

import io
import re
from dataclasses import dataclass, field

import pikepdf


@dataclass
class LayerInfo:
    ocgs: list = field(default_factory=list)          # [{"name", "default_on", "used_on_page"}]
    spots: list = field(default_factory=list)         # Separation-namn på sidan


def _ocg_name(o) -> str:
    try:
        return str(o.get("/Name", ""))
    except Exception:  # noqa: BLE001
        return ""


def list_layers(pdf_bytes: bytes, page_index: int = 0) -> LayerInfo:
    pdf = pikepdf.open(io.BytesIO(pdf_bytes))
    info = LayerInfo()
    root = pdf.Root
    off = set()
    if "/OCProperties" in root:
        ocp = root.OCProperties
        d = ocp.get("/D", {})
        for o in d.get("/OFF", []):
            off.add(o.objgen)
        used = set()
        page = pdf.pages[page_index]
        props = page.Resources.get("/Properties", {}) if "/Resources" in page else {}
        for _, v in props.items():
            for g in _ocgs_of(v):
                used.add(g.objgen)
        for _, xo in (page.Resources.get("/XObject", {}) if "/Resources" in page else {}).items():
            if "/OC" in xo:
                for g in _ocgs_of(xo.OC):
                    used.add(g.objgen)
        for g in ocp.get("/OCGs", []):
            info.ocgs.append({"name": _ocg_name(g), "default_on": g.objgen not in off,
                              "used_on_page": g.objgen in used})
    page = pdf.pages[page_index]
    res = page.Resources if "/Resources" in page else None
    if res is not None and "/ColorSpace" in res:
        for _, val in res.ColorSpace.items():
            try:
                if isinstance(val, pikepdf.Array) and str(val[0]) == "/Separation":
                    info.spots.append(str(val[1]).lstrip("/"))
                elif isinstance(val, pikepdf.Array) and str(val[0]) == "/DeviceN":
                    info.spots.extend(str(n).lstrip("/") for n in val[1])
            except Exception:  # noqa: BLE001
                continue
    info.spots = sorted(set(info.spots) - {"All", "None"})
    return info


def _ocgs_of(obj) -> list:
    """OCG:er som ett /OC-värde (OCG eller OCMD) refererar till."""
    try:
        t = str(obj.get("/Type", ""))
    except Exception:  # noqa: BLE001
        return []
    if t == "/OCG":
        return [obj]
    if t == "/OCMD":
        g = obj.get("/OCGs")
        if g is None:
            return []
        return list(g) if isinstance(g, pikepdf.Array) else [g]
    return []


def _compile(rules) -> list:
    return [re.compile(r if any(c in r for c in ".*+?[]()|^$\\") else "^" + re.escape(r) + "$", re.I)
            for r in (rules or [])]


def _matches(name: str, pats) -> bool:
    return any(p.search(name) for p in pats)


def decide(include=None, exclude=None):
    """Funktion namn -> synligt? enligt include/exclude."""
    inc, exc = _compile(include), _compile(exclude)

    def visible(name: str) -> bool:
        if inc and not _matches(name, inc):
            return False
        return not (exc and _matches(name, exc))
    return visible


def _oc_visible(oc, visible) -> bool:
    """Synlighet för ett /OC-värde. OCMD: AnyOn (standard), AllOn, AnyOff, AllOff."""
    groups = _ocgs_of(oc)
    if not groups:
        return True
    states = [visible(_ocg_name(g)) for g in groups]
    policy = "/AnyOn"
    try:
        if str(oc.get("/Type", "")) == "/OCMD":
            policy = str(oc.get("/P", "/AnyOn"))
    except Exception:  # noqa: BLE001
        pass
    return {"/AllOn": all(states), "/AnyOff": not all(states), "/AllOff": not any(states)}.get(policy, any(states))


def _filter_stream(ops, resources, visible, pdf, seen_forms) -> list:
    props = resources.get("/Properties", {}) if resources is not None else {}
    xobjs = resources.get("/XObject", {}) if resources is not None else {}
    out = []
    stack: list[bool] = []          # per markerat innehåll: dolt?
    hidden_depth = 0
    for operands, op in ops:
        o = str(op)
        if o in ("BDC", "BMC"):
            hide = False
            if o == "BDC" and len(operands) >= 2 and str(operands[0]) == "/OC":
                ref = operands[1]
                oc = props.get(str(ref)) if isinstance(ref, pikepdf.Name) else ref
                if oc is not None and not _oc_visible(oc, visible):
                    hide = True
            stack.append(hide)
            if hide or hidden_depth:
                hidden_depth += 1
                continue
        elif o == "EMC":
            hide = stack.pop() if stack else False
            if hidden_depth:
                hidden_depth -= 1
                continue
        elif hidden_depth:
            continue
        elif o == "Do" and operands:
            xo = xobjs.get(str(operands[0]))
            if xo is not None:
                if "/OC" in xo and not _oc_visible(xo.OC, visible):
                    continue
                if str(xo.get("/Subtype", "")) == "/Form" and xo.objgen not in seen_forms:
                    seen_forms.add(xo.objgen)
                    sub = _filter_stream(pikepdf.parse_content_stream(xo), xo.get("/Resources"), visible, pdf,
                                         seen_forms)
                    xo.write(pikepdf.unparse_content_stream(sub))
        out.append(pikepdf.ContentStreamInstruction(operands, op))
    return out


def filter_pdf(pdf_bytes: bytes, page_index: int = 0, include=None, exclude=None) -> tuple[bytes, dict]:
    """Ta bort OCG-lager som inte ska jämföras. Returnerar (ny PDF, {lager: med?})."""
    if not include and not exclude:
        return pdf_bytes, {}
    visible = decide(include, exclude)
    pdf = pikepdf.open(io.BytesIO(pdf_bytes))
    decision = {}
    if "/OCProperties" in pdf.Root:
        ocp = pdf.Root.OCProperties
        on, off = [], []
        for g in ocp.get("/OCGs", []):
            v = visible(_ocg_name(g))
            decision[_ocg_name(g)] = v
            (on if v else off).append(g)
        d = ocp.get("/D")
        if d is not None:
            d.ON = pikepdf.Array(on)
            d.OFF = pikepdf.Array(off)
            d.BaseState = pikepdf.Name("/ON")
            if "/AS" in d:
                del d["/AS"]
    page = pdf.pages[page_index]
    ops = pikepdf.parse_content_stream(page)
    res = page.Resources if "/Resources" in page else None
    new = _filter_stream(ops, res, visible, pdf, set())
    page.Contents = pdf.make_stream(pikepdf.unparse_content_stream(new))
    buf = io.BytesIO()
    pdf.save(buf)
    return buf.getvalue(), decision


def spot_rules(include_spots=None, exclude_spots=None) -> dict:
    """Extra regler till pdfspots: uteslutna dekorfärger tas bort ur renderingen."""
    if not exclude_spots:
        return {}
    pats = [r if any(c in r for c in ".*+?[]()|^$\\") else "^" + re.escape(r) + "$" for r in exclude_spots]
    return {"excluded": "(?i)" + "|".join(f"(?:{p})" for p in pats)}
