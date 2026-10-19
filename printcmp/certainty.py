"""Säkerhetsgrad per skillnad: från bekräftad till möjlig.

Ingen automatisk jämförelse kan garantera 100 %. Graderingen anger hur starkt
underlaget är och *varför*, så att granskaren kan börja med det säkra och lägga sin
tid på det osäkra:

* confirmed – bekräftad av oberoende belägg: avkodat innehåll skiljer (streckkod,
  punktskrift), samma avvikelse på samma ställe i (nästan) alla etiketter med tydlig
  signal, eller PDF mot PDF (deterministisk rendering, inget brus).
* high      – tydlig signal långt över tröskeln, utanför veck.
* probable  – signal över tröskeln men nära den, eller i ett område med sämre underlag.
* possible  – osäker zon (veck, registrering), svag signal eller färg i okalibrerat läge.
"""
from __future__ import annotations

import numpy as np

LEVELS = ("confirmed", "high", "probable", "possible")
BASE = {"confirmed": 95, "high": 80, "probable": 60, "possible": 30}


def _down(level: str) -> str:
    return LEVELS[min(len(LEVELS) - 1, LEVELS.index(level) + 1)]


def _near_fold(d: dict, inst_folds: dict, mm: float = 4.0) -> bool:
    if not d.get("bbox_mm"):
        return False
    x, y, w, h = d["bbox_mm"]
    c = np.array([x + w / 2, y + h / 2])
    r = max(w, h) / 2
    for i in d.get("instances", []):
        for f in inst_folds.get(i, []):
            a, b = np.array(f["p0_mm"], float), np.array(f["p1_mm"], float)
            e = b - a
            t = float(np.dot(c - a, e) / max(np.dot(e, e), 1e-9))
            if np.linalg.norm(a + t * e - c) <= max(mm, f.get("band_mm", 0) + 1.0) + r:
                return True
    return False


def assess(d: dict, n_inst: int, tol: dict, sample_kind: str, inst_folds: dict,
           color_calibrated: bool = False) -> dict:
    kind = d["kind"]
    reasons: list[str] = []
    k = len(d.get("instances") or [])
    m = d.get("metrics") or {}
    bonus = 0.0

    if kind == "Uncertain":
        cause = {"fold": "veck", "registration": "osäker registrering"}.get(d.get("cause"), d.get("cause") or "")
        return {"level": "possible", "score": 20, "reasons": [f"Kan inte bedömas här ({cause})"]}

    if d.get("reason") == "instance_not_found":
        return {"level": "confirmed", "score": 100, "reasons": ["Etiketter saknas på arket"]}

    if sample_kind == "pdf":
        # PDF mot PDF: båda renderas exakt, så varje skillnad är verklig
        level = "confirmed"
        for tc in (d.get("text_change") or [])[:2]:
            if tc.get("kind") == "Style":
                reasons.append(f"Textlager: samma text, {tc.get('size_from')} → {tc.get('size_to')} pt"
                               + (f", {tc.get('font_from')} → {tc.get('font_to')}" if tc.get("font_from") != tc.get("font_to") else ""))
            else:
                reasons.append(f"Textlager: \u201d{tc.get('from', '')}\u201d → \u201d{tc.get('to', '')}\u201d")
        if d.get("text_only"):
            reasons.append("Syns inte i tryck – bara i textlagret")
        reasons.append("PDF mot PDF: exakt rendering utan brus")
        if kind == "Barcode" and d.get("barcode", {}).get("read"):
            reasons.append(f"Avkodat innehåll skiljer: {d['barcode']['expected']} → {d['barcode']['read']}")
        return {"level": level, "score": 99, "reasons": reasons}

    if kind == "Barcode":
        bc = d.get("barcode", {})
        if bc.get("read"):
            return {"level": "confirmed", "score": 99,
                    "reasons": [f"Avkodat innehåll skiljer: väntat {bc.get('expected')}, läst {bc.get('read')}"]}
        level, bonus = "high", 0
        reasons.append("Koden kunde inte avkodas i provet")
    elif kind == "Braille":
        bi = d.get("braille", {})
        if d.get("cause") == "embossing_missing":
            level = "high"
            reasons.append("Ingen präglingssignal i hela raden")
        else:
            c, mg = bi.get("contrast", 0) or 0, bi.get("margin", 0) or 0
            if mg >= 0.5 and c >= 2.5:
                level = "high"
            elif mg >= 0.2:
                level = "probable"
            else:
                level = "possible"
            reasons.append(f"Punktskrift läst ”{bi.get('read', '').strip() or '–'}”, väntat "
                           f"”{bi.get('expected', '').strip() or '–'}” (kontrast {c:.1f}, marginal {mg:.1f})")
            bonus = 10 * min(1.0, mg)
    elif kind == "Color":
        de = m.get("delta_e") or 0
        if color_calibrated:
            level = "high" if de >= 2 * tol["delta_e"] else "probable"
        else:
            level = "probable" if de >= 2 * tol["delta_e"] else "possible"
            reasons.append("Färg mätt med okalibrerad scanner")
        reasons.append(f"ΔE00 {de:.1f} (tröskel {tol['delta_e']:.0f})")
        bonus = min(10, de - tol["delta_e"])
    else:
        r = (m.get("delta_l") or 0) / tol["struct_dl"]
        area = d.get("area_mm2") or 0
        if r >= 2.5 and area >= 0.1:
            level = "high"
        elif r >= 1.5:
            level = "probable"
        else:
            level = "possible"
        reasons.append(f"Ljushetsavvikelse {m.get('delta_l') or 0:.0f} L* ({r:.1f} × tröskeln), yta {area:.2f} mm²")
        bonus = 10 * min(1.0, r / 4)
        if d.get("source") == "consensus":
            level = max(level, "probable", key=LEVELS.index)
            reasons.append("Hittad bara vid jämförelse mellan etiketter")

    # samma avvikelse oberoende i (nästan) alla etiketter: brus upprepar sig inte på samma ställe
    repeated = d.get("systematic") and n_inst >= 3 and k >= max(3, int(np.ceil(0.75 * n_inst))) and kind != "Color"
    if repeated:
        # Brus och scanartefakter hamnar inte på samma etikettposition i alla etiketter.
        # Med tydlig signal är upprepningen ett oberoende belägg; med svag signal kan det
        # fortfarande vara en systematisk skillnad mellan rendering och tryck.
        strong = level in ("confirmed", "high") or kind in ("Braille", "Barcode") or \
            ((m.get("delta_l") or 0) >= 1.5 * tol["struct_dl"])
        level = "confirmed" if strong else "high"
        reasons.insert(0, f"Samma avvikelse oberoende i {k} av {n_inst} etiketter")
    elif d.get("systematic"):
        reasons.insert(0, f"Avvikelsen finns i {k} av {n_inst} etiketter")

    if level != "confirmed" and not repeated and _near_fold(d, inst_folds):
        level = _down(level)
        reasons.append("Nära ett veck")

    return {"level": level, "score": int(round(min(99, BASE[level] + bonus))), "reasons": reasons}
