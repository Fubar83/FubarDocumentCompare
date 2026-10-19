"""Textjämförelse mellan två PDF:ers textlager (bara PDF mot PDF).

Orden tas ur textlagret med position, typsnitt och storlek, ordnas i läsordning
(rader uppifrån och ned, vänster till höger) och jämförs som en sekvens:

* ``Changed``  – ord ersatt (t.ex. "10 %" -> "18 %"), med teckenvis skillnad
* ``Missing``  – ord som bara finns i referensen
* ``Added``    – ord som bara finns i provet
* ``Moved``    – samma ord borttaget på ett ställe och tillagt på ett annat
* ``Style``    – samma ord men annat typsnitt eller annan storlek

Positioner rapporteras i referensens koordinater; provets ord förs över med
registreringen (H: ref px -> prov px).

Textjämförelsen kompletterar den grafiska: text som är konturerad eller ligger i en
bild syns inte i textlagret, och ett fel i ToUnicode-tabellen syns inte grafiskt.
"""
from __future__ import annotations

import difflib
import re
import unicodedata
from dataclasses import dataclass

import numpy as np
import pypdfium2 as pdfium
import pypdfium2.raw as pdfium_c

from .render import PageGeometry


@dataclass
class Word:
    text: str
    bbox: tuple          # px (x0, y0, x1, y1) i den egna sidans rendering
    size: float          # punkter
    font: str
    line: int = -1


def _font_name(tp, i) -> str:
    try:
        import ctypes
        buf = ctypes.create_string_buffer(256)
        flags = ctypes.c_int(0)
        n = pdfium_c.FPDFText_GetFontInfo(tp.raw, i, buf, 256, ctypes.byref(flags))
        name = buf.value.decode("utf-8", "replace") if n else ""
        return re.sub(r"^[A-Z]{6}\+", "", name)        # delmängdsprefix (ABCDEF+Arial)
    except Exception:  # noqa: BLE001
        return ""


def extract_words(pdf_bytes: bytes, page_index: int, geom: PageGeometry) -> list[Word]:
    doc = pdfium.PdfDocument(pdf_bytes)
    words: list[Word] = []
    try:
        page = doc[page_index]
        tp = page.get_textpage()
        n = tp.count_chars()
        cur, box, sizes, font = "", None, [], ""
        last_r = last_b = None

        def flush():
            nonlocal cur, box, sizes, font
            if cur.strip() and box is not None:
                l, b, r, t = box
                x0, y0 = geom.pdf_to_px(l, t)
                x1, y1 = geom.pdf_to_px(r, b)
                words.append(Word(unicodedata.normalize("NFC", cur.strip()), (float(x0), float(y0), float(x1), float(y1)),
                                  float(np.median(sizes)) if sizes else 0.0, font))
            cur, box, sizes, font = "", None, [], ""

        for i in range(n):
            ch = tp.get_text_range(i, 1)
            l, b, r, t = tp.get_charbox(i, loose=False)
            if ch.isspace() or ch in ("\r", "\n", "\x02") or (r - l) <= 0:
                flush()
                last_r = None
                continue
            size = max(t - b, (box[3] - box[1]) if box is not None else 0.0, 1.0)
            if last_r is not None and (l - last_r > 0.6 * size or abs(b - last_b) > 0.6 * size):
                flush()
            cur += ch
            box = (l, b, r, t) if box is None else (min(box[0], l), min(box[1], b), max(box[2], r), max(box[3], t))
            try:
                sizes.append(float(pdfium_c.FPDFText_GetFontSize(tp.raw, i)))
            except Exception:  # noqa: BLE001
                pass
            if not font:
                font = _font_name(tp, i)
            last_r, last_b = r, b
        flush()
        tp.close()
    finally:
        doc.close()
    return _reading_order(words)


def _reading_order(words: list[Word]) -> list[Word]:
    """Gruppera i rader (överlappande höjd) och sortera rader uppifrån, ord vänster->höger.

    Kolumner: ord med en stor lucka till närmaste granne på raden bildar egna block, så att
    två textspalter bredvid varandra inte vävs ihop rad för rad.
    """
    if not words:
        return words
    ws = sorted(words, key=lambda w: (w.bbox[1] + w.bbox[3]) / 2)
    lines: list[list[Word]] = []
    for w in ws:
        cy, h = (w.bbox[1] + w.bbox[3]) / 2, w.bbox[3] - w.bbox[1]
        for ln in lines:
            ly = np.mean([(x.bbox[1] + x.bbox[3]) / 2 for x in ln])
            lh = np.mean([x.bbox[3] - x.bbox[1] for x in ln])
            if abs(cy - ly) < 0.5 * max(h, lh):
                ln.append(w)
                break
        else:
            lines.append([w])
    out = []
    for k, ln in enumerate(sorted(lines, key=lambda ln: np.mean([(x.bbox[1] + x.bbox[3]) / 2 for x in ln]))):
        for w in sorted(ln, key=lambda w: w.bbox[0]):
            w.line = k
            out.append(w)
    return out


def _map_box(box, Hinv):
    x0, y0, x1, y1 = box
    P = np.array([[x0, y0, 1], [x1, y0, 1], [x1, y1, 1], [x0, y1, 1]], float) @ Hinv.T
    P = P[:, :2] / P[:, 2:3]
    return (float(P[:, 0].min()), float(P[:, 1].min()), float(P[:, 0].max()), float(P[:, 1].max()))


def _union(boxes):
    return (min(b[0] for b in boxes), min(b[1] for b in boxes), max(b[2] for b in boxes), max(b[3] for b in boxes))


def char_diff(a: str, b: str) -> str:
    """Kompakt teckenvis skillnad: 'Fet[t→]' eller '1[0→8] %'."""
    sm = difflib.SequenceMatcher(None, a, b, autojunk=False)
    out = []
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == "equal":
            out.append(a[i1:i2])
        else:
            out.append(f"[{a[i1:i2]}→{b[j1:j2]}]")
    return "".join(out)


def compare(ref_words: list[Word], smp_words: list[Word], H: np.ndarray, ppm: float,
            size_tol: float = 0.05) -> list[dict]:
    """Jämför två ordlistor. H: ref px -> prov px. Returnerar skillnader i ref-koordinater."""
    Hinv = np.linalg.inv(H)
    smp_in_ref = [_map_box(w.bbox, Hinv) for w in smp_words]
    a = [w.text for w in ref_words]
    b = [w.text for w in smp_words]
    sm = difflib.SequenceMatcher(None, a, b, autojunk=False)
    diffs: list[dict] = []

    def mk(kind, box, frm, to, extra=None):
        x0, y0, x1, y1 = box
        d = {"kind": kind, "bbox_px": [int(x0), int(y0), int(x1 - x0), int(y1 - y0)],
             "bbox_mm": [x0 / ppm, y0 / ppm, (x1 - x0) / ppm, (y1 - y0) / ppm],
             "text": {"from": frm, "to": to}}
        if extra:
            d["text"].update(extra)
        diffs.append(d)

    removed, added = [], []
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == "equal":
            for i, j in zip(range(i1, i2), range(j1, j2)):
                rw, sw = ref_words[i], smp_words[j]
                if rw.size and sw.size and abs(rw.size - sw.size) > size_tol * rw.size or \
                        (rw.font and sw.font and rw.font != sw.font):
                    mk("Style", _union([rw.bbox, smp_in_ref[j]]), rw.text, sw.text,
                       {"font_from": rw.font, "font_to": sw.font,
                        "size_from": round(rw.size, 2), "size_to": round(sw.size, 2)})
        elif tag == "replace":
            ra, sb = ref_words[i1:i2], list(range(j1, j2))
            if len(ra) == len(sb):
                for rw, j in zip(ra, sb):
                    mk("Changed", _union([rw.bbox, smp_in_ref[j]]), rw.text, smp_words[j].text,
                       {"diff": char_diff(rw.text, smp_words[j].text)})
            else:
                frm = " ".join(w.text for w in ra)
                to = " ".join(smp_words[j].text for j in sb)
                mk("Changed", _union([w.bbox for w in ra] + [smp_in_ref[j] for j in sb]), frm, to,
                   {"diff": char_diff(frm, to)})
        elif tag == "delete":
            removed.extend(range(i1, i2))
        elif tag == "insert":
            added.extend(range(j1, j2))

    # flyttade ord: borttaget här, tillagt där (samma text)
    used_add = set()
    for i in removed:
        rw = ref_words[i]
        j = next((j for j in added if j not in used_add and smp_words[j].text == rw.text), None)
        if j is not None:
            used_add.add(j)
            c0 = np.array([(rw.bbox[0] + rw.bbox[2]) / 2, (rw.bbox[1] + rw.bbox[3]) / 2])
            sb_ = smp_in_ref[j]
            c1 = np.array([(sb_[0] + sb_[2]) / 2, (sb_[1] + sb_[3]) / 2])
            mk("Moved", _union([rw.bbox, sb_]), rw.text, rw.text,
               {"shift_mm": round(float(np.linalg.norm(c1 - c0)) / ppm, 2), "from_bbox_mm":
                [rw.bbox[0] / ppm, rw.bbox[1] / ppm, (rw.bbox[2] - rw.bbox[0]) / ppm, (rw.bbox[3] - rw.bbox[1]) / ppm]})
        else:
            mk("Missing", rw.bbox, rw.text, "")
    for j in added:
        if j not in used_add:
            mk("Added", smp_in_ref[j], "", smp_words[j].text)
    return _merge_runs(diffs, ppm)


def _merge_runs(diffs: list[dict], ppm: float) -> list[dict]:
    """Slå ihop intilliggande tillagda/borttagna ord på samma rad till en skillnad."""
    out: list[dict] = []
    for d in sorted(diffs, key=lambda d: (d["kind"], round(d["bbox_mm"][1]), d["bbox_mm"][0])):
        if out and d["kind"] == "Style" and out[-1]["kind"] == "Style" and \
                out[-1]["text"].get("size_to") == d["text"].get("size_to") and \
                out[-1]["text"].get("font_to") == d["text"].get("font_to"):
            p = out[-1]
            px, py, pw, ph = p["bbox_mm"]
            x, y, w, h = d["bbox_mm"]
            if abs((py + ph / 2) - (y + h / 2)) < 0.5 * max(ph, h):
                x0, y0 = min(px, x), min(py, y)
                x1, y1 = max(px + pw, x + w), max(py + ph, y + h)
                p["bbox_mm"] = [x0, y0, x1 - x0, y1 - y0]
                p["bbox_px"] = [int(x0 * ppm), int(y0 * ppm), int((x1 - x0) * ppm), int((y1 - y0) * ppm)]
                p["text"]["from"] = p["text"]["to"] = p["text"]["from"] + " " + d["text"]["from"]
                continue
        if out and d["kind"] in ("Added", "Missing") and out[-1]["kind"] == d["kind"]:
            p = out[-1]
            px, py, pw, ph = p["bbox_mm"]
            x, y, w, h = d["bbox_mm"]
            same_line = abs((py + ph / 2) - (y + h / 2)) < 0.5 * max(ph, h)
            if same_line and x - (px + pw) < 2.5 * max(ph, h):
                x0, y0 = min(px, x), min(py, y)
                x1, y1 = max(px + pw, x + w), max(py + ph, y + h)
                p["bbox_mm"] = [x0, y0, x1 - x0, y1 - y0]
                p["bbox_px"] = [int(x0 * ppm), int(y0 * ppm), int((x1 - x0) * ppm), int((y1 - y0) * ppm)]
                key = "from" if d["kind"] == "Missing" else "to"
                p["text"][key] = (p["text"][key] + " " + d["text"][key]).strip()
                continue
        out.append(d)
    return out
