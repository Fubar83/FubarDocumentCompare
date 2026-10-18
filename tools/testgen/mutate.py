"""Felkatalog (avsnitt 9.3): mutationer på beskrivningsnivå med exakt facit.

Varje mutation returnerar en ny spec och ett facitfel:
  {id, kind, accept, systematic, element, detail, bbox_ref_mm}
`kind` är förväntad klass; `accept` är klasser som räknas som träff i detektion.
"""
from __future__ import annotations

import copy

import numpy as np

from .labelpdf import char_bbox_mm, element_bbox_mm, text_width_mm

LOOKALIKE = {
    "O": "0", "0": "O", "o": "e", "e": "c", "c": "o", "l": "1", "1": "l", "I": "l", "i": "j", "a": "o",
    "n": "m", "m": "n", "u": "v", "v": "u", "r": "n", "t": "f", "f": "t", "S": "5", "5": "S", "B": "8",
    "8": "B", "s": "z", "k": "h", "h": "b", "d": "b", "b": "d", "g": "q", "p": "q", "E": "F", "F": "E",
    "H": "N", "N": "H", "V": "Y", "2": "7", "3": "8", "4": "9", "6": "8", "9": "4", "7": "1",
}
DIACRITIC = {"å": "a", "ä": "a", "ö": "o", "Å": "A", "Ä": "A", "Ö": "O", "é": "e", "ó": "o"}
STRUCT_ANY = ["Missing", "Added", "Changed", "Moved", "Shape", "Color"]


def _union(a, b):
    x0, y0 = min(a[0], b[0]), min(a[1], b[1])
    x1, y1 = max(a[0] + a[2], b[0] + b[2]), max(a[1] + a[3], b[1] + b[3])
    return [x0, y0, x1 - x0, y1 - y0]


def _texts(spec):
    return [e for e in spec["elements"] if e["type"] == "text"]


def _inside(spec, box, margin=1.0):
    W, H = spec["size_mm"]
    x, y, w, h = box
    return x >= margin and y >= margin and x + w <= W - margin and y + h <= H - margin


def char_change(spec, rng):
    cands = [(e, i) for e in _texts(spec) for i, ch in enumerate(e["text"]) if ch in LOOKALIKE]
    if not cands:
        return None
    e, i = cands[rng.integers(len(cands))]
    s = copy.deepcopy(spec)
    el = next(x for x in s["elements"] if x["id"] == e["id"])
    old_box = char_bbox_mm(el, i)
    old = el["text"][i]
    el["text"] = el["text"][:i] + LOOKALIKE[old] + el["text"][i + 1:]
    new_box = char_bbox_mm(el, i)
    # om bredden ändras flyttas resten av raden: facit täcker till radens slut
    if abs(old_box[2] - new_box[2]) > 0.05:
        end = char_bbox_mm(el, i, len(el["text"]))
        box = _union(_union(old_box, new_box), end)
    else:
        box = _union(old_box, new_box)
    return s, {"kind": "Changed", "accept": STRUCT_ANY, "element": e["id"],
               "detail": {"from": old, "to": LOOKALIKE[old], "index": int(i)}, "bbox_ref_mm": box}


def diacritic(spec, rng):
    cands = [(e, i) for e in _texts(spec) for i, ch in enumerate(e["text"]) if ch in DIACRITIC]
    if not cands:
        return None
    e, i = cands[rng.integers(len(cands))]
    s = copy.deepcopy(spec)
    el = next(x for x in s["elements"] if x["id"] == e["id"])
    old_box = char_bbox_mm(el, i)
    old = el["text"][i]
    el["text"] = el["text"][:i] + DIACRITIC[old] + el["text"][i + 1:]
    box = _union(old_box, char_bbox_mm(el, i))
    if abs(old_box[2] - box[2]) > 0.05:
        box = _union(box, char_bbox_mm(el, i, len(el["text"])))
    return s, {"kind": "Changed", "accept": STRUCT_ANY, "element": e["id"],
               "detail": {"from": old, "to": DIACRITIC[old], "index": int(i)}, "bbox_ref_mm": box}


def word_delete(spec, rng):
    cands = [e for e in _texts(spec) if len(e["text"].split(" ")) >= 3]
    if not cands:
        return None
    e = cands[rng.integers(len(cands))]
    words = e["text"].split(" ")
    k = int(rng.integers(1, len(words)))
    start = len(" ".join(words[:k]))
    s = copy.deepcopy(spec)
    el = next(x for x in s["elements"] if x["id"] == e["id"])
    old_end = char_bbox_mm(el, start, len(el["text"]))
    el["text"] = " ".join(words[:k] + words[k + 1:])
    new_end = char_bbox_mm(el, min(start, len(el["text"]) - 1), len(el["text"]))
    return s, {"kind": "Missing", "accept": STRUCT_ANY, "element": e["id"],
               "detail": {"removed": words[k]}, "bbox_ref_mm": _union(old_end, new_end)}


def append_text(spec, rng):
    W = spec["size_mm"][0]
    cands = []
    for e in _texts(spec):
        add = " " + "".join(rng.choice(list("abcdefghkmnprstuvxyz0123456789"), int(rng.integers(2, 6))))
        if e["pos_mm"][0] + text_width_mm(e["font"], e["size_pt"], e["text"] + add) < W - 3:
            cands.append((e, add))
    if not cands:
        return None
    e, add = cands[rng.integers(len(cands))]
    s = copy.deepcopy(spec)
    el = next(x for x in s["elements"] if x["id"] == e["id"])
    n = len(el["text"])
    el["text"] += add
    box = char_bbox_mm(el, n, len(el["text"]))
    return s, {"kind": "Added", "accept": STRUCT_ANY, "element": e["id"], "detail": {"added": add},
               "bbox_ref_mm": box}


def font_size(spec, rng):
    W = spec["size_mm"][0]
    cands = [e for e in _texts(spec)]
    rng.shuffle(cands)
    for e in cands:
        f = 1 + rng.choice([-1, 1]) * rng.uniform(0.08, 0.15)
        s = copy.deepcopy(spec)
        el = next(x for x in s["elements"] if x["id"] == e["id"])
        old = element_bbox_mm(el)
        el["size_pt"] = round(el["size_pt"] * f, 2)
        new = element_bbox_mm(el)
        if new[0] + new[2] < W - 2:
            return s, {"kind": "Shape", "accept": STRUCT_ANY, "element": e["id"],
                       "detail": {"factor": round(float(f), 3)}, "bbox_ref_mm": _union(old, new)}
    return None


def move(spec, rng):
    movable = [e for e in spec["elements"] if e["type"] in ("text", "circle", "poly", "ean13", "qr")]
    rng.shuffle(movable)
    for e in movable:
        d = rng.uniform(0.3, 2.5)
        a = rng.uniform(0, 2 * np.pi)
        dx, dy = float(d * np.cos(a)), float(d * np.sin(a))
        s = copy.deepcopy(spec)
        el = next(x for x in s["elements"] if x["id"] == e["id"])
        old = element_bbox_mm(el)
        if el["type"] == "text":
            el["pos_mm"] = [el["pos_mm"][0] + dx, el["pos_mm"][1] + dy]
        elif el["type"] == "circle":
            el["center_mm"] = [el["center_mm"][0] + dx, el["center_mm"][1] + dy]
        elif el["type"] == "poly":
            el["points_mm"] = [[x + dx, y + dy] for x, y in el["points_mm"]]
        else:
            x, y, w, h = el["rect_mm"]
            el["rect_mm"] = [x + dx, y + dy, w, h]
        new = element_bbox_mm(el)
        if _inside(spec, new, 1.5):
            return s, {"kind": "Moved", "accept": STRUCT_ANY + ["Barcode"], "element": e["id"],
                       "detail": {"dx_mm": round(dx, 3), "dy_mm": round(dy, 3), "shift_mm": round(float(d), 3)},
                       "bbox_ref_mm": _union(old, new)}
    return None


def delete(spec, rng):
    cands = [e for e in spec["elements"] if e["type"] in ("circle", "poly", "text")
             and e["id"] not in ("title",)]
    if not cands:
        return None
    e = cands[rng.integers(len(cands))]
    s = copy.deepcopy(spec)
    s["elements"] = [x for x in s["elements"] if x["id"] != e["id"]]
    return s, {"kind": "Missing", "accept": STRUCT_ANY, "element": e["id"], "detail": {},
               "bbox_ref_mm": element_bbox_mm(e)}


def color_shift(spec, rng):
    W, H = spec["size_mm"]

    def small(e):  # färgfel på ytor > 40 % av etiketten kalibreras bort av prototypens självkalibrering
        b = element_bbox_mm(e)
        return b[2] * b[3] < 0.4 * W * H

    cands = [e for e in spec["elements"] if e["type"] in ("rect", "circle", "poly") and e.get("color")
             and sum(e["color"]["cmyk"]) > 0.05 and small(e)]
    if not cands:
        return None
    e = cands[rng.integers(len(cands))]
    s = copy.deepcopy(spec)
    el = next(x for x in s["elements"] if x["id"] == e["id"])
    cmyk = list(el["color"]["cmyk"])
    ch = int(rng.integers(0, 3))
    delta = float(rng.choice([-1, 1]) * rng.uniform(0.12, 0.3))
    cmyk[ch] = float(np.clip(cmyk[ch] + delta, 0, 1))
    if abs(cmyk[ch] - el["color"]["cmyk"][ch]) < 0.08:
        cmyk[ch] = float(np.clip(el["color"]["cmyk"][ch] - delta, 0, 1))
    el["color"] = dict(el["color"], cmyk=cmyk)
    return s, {"kind": "Color", "accept": STRUCT_ANY, "element": e["id"],
               "detail": {"channel": "CMY"[ch], "from": e["color"]["cmyk"][ch], "to": cmyk[ch]},
               "bbox_ref_mm": element_bbox_mm(e)}


def barcode_digit(spec, rng):
    cands = [e for e in spec["elements"] if e["type"] == "ean13"]
    if not cands:
        return None
    e = cands[0]
    s = copy.deepcopy(spec)
    el = next(x for x in s["elements"] if x["id"] == e["id"])
    d = list(el["data"][:12])
    i = int(rng.integers(1, 12))
    d[i] = str((int(d[i]) + int(rng.integers(1, 9))) % 10)
    el["data"] = "".join(d)
    return s, {"kind": "Barcode", "accept": ["Barcode", "Changed", "Shape", "Missing", "Added"],
               "element": e["id"], "detail": {"from": e["data"], "to": el["data"]},
               "bbox_ref_mm": list(e["rect_mm"])}


CATALOG = {
    "char_change": char_change, "diacritic": diacritic, "word_delete": word_delete, "append_text": append_text,
    "font_size": font_size, "move": move, "delete": delete, "color_shift": color_shift,
    "barcode_digit": barcode_digit,
}


def _overlaps(a, b, pad=1.0):
    return not (a[0] - pad > b[0] + b[2] or b[0] - pad > a[0] + a[2] or
                a[1] - pad > b[1] + b[3] or b[1] - pad > a[1] + a[3])


def mutate(spec: dict, rng: np.random.Generator, n: int, allowed=None):
    """Applicera n mutationer på olika element utan överlappande facitrutor."""
    names = list(allowed or CATALOG)
    defects = []
    used_elements = set()
    tries = 0
    while len(defects) < n and tries < 60:
        tries += 1
        name = names[rng.integers(len(names))]
        r = CATALOG[name](spec, rng)
        if r is None:
            continue
        s, d = r
        if d["element"] in used_elements or d["element"] in ("lot",):
            continue
        if any(_overlaps(d["bbox_ref_mm"], x["bbox_ref_mm"]) for x in defects):
            continue
        spec = s
        used_elements.add(d["element"])
        d.update({"id": f"a{len(defects) + 1}", "mutation": name, "systematic": True,
                  "bbox_ref_mm": [round(float(v), 3) for v in d["bbox_ref_mm"]]})
        defects.append(d)
    return spec, defects
