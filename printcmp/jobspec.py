"""JobSpec: inläsning, validering, standardvärden och toleransprofiler (avsnitt 7)."""
from __future__ import annotations

import copy
import json
from pathlib import Path

import jsonschema

SCHEMA_ID = "printcmp.jobspec/1"

PROFILES = {
    # Toleranser i mm, ΔE00 och L*-enheter. Startvärden; kalibreras mot testmängden.
    "label_standard": {
        "min_blob_mm2": 0.04,
        "struct_dl": 20.0,         # L*-avvikelse utanför grannskapets min/max
        "struct_dl_strong": 35.0,
        "struct_dl_text": 15.0,      # lägre tröskel inom textobjekt
        "min_text_blob_mm2": 0.015,
        "text_small_mm": 2.6,        # ordhöjd upp till vilken textkänsligheten gäller
        "ssim": 0.35,
        "edge_radius_mm": 0.15,    # tillåten kantförskjutning (r_edge)
        "delta_e": 8.0,             # okalibrerad scanner; med IT8-kalibrering är 3–5 rimligt
        "min_color_blob_mm2": 2.0,  # färgfel är ytor; mindre fläckar är raster/moiré
        "color_window_mm": 1.5,     # större än typisk moiréperiod (≈ 1–1,3 mm)
        "local_cell_mm": 6.0,
        "max_local_shift_mm": 0.6,
        "fold_band_mm": 1.6,
        "dieline_margin_mm": 0.6,  # stanslinjens lägestolerans mot trycket
        "merge_mm": 0.5,
        "descreen": True,
        "normalize": True,
        "illumination_flatten": False,  # planskanner har jämn belysning; veck hanteras separat
        "local_registration": True,
        "fold_detection": True,
    },
    "label_strict": {
        "extends": "label_standard",
        "min_blob_mm2": 0.02, "struct_dl": 15.0, "delta_e": 4.0, "edge_radius_mm": 0.10,
    },
    "leaflet": {
        "extends": "label_standard",
        "min_blob_mm2": 0.10, "struct_dl": 20.0, "delta_e": 6.0, "edge_radius_mm": 0.20,
        "max_local_shift_mm": 1.0,
    },
    "pdf_vs_pdf": {
        "extends": "label_standard",
        "min_blob_mm2": 0.005, "min_text_blob_mm2": 0.005, "struct_dl": 8.0, "struct_dl_text": 8.0, "struct_dl_strong": 20.0, "ssim": 0.9,
        "edge_radius_mm": 0.03, "delta_e": 1.5, "min_color_blob_mm2": 0.05, "color_window_mm": 0.6,
        "descreen": False, "normalize": False, "local_registration": False, "fold_detection": False,
    },
}

SCHEMA = {
    "type": "object",
    "additionalProperties": False,
    "required": ["schema", "reference", "sample"],
    "properties": {
        "schema": {"const": SCHEMA_ID},
        "seed": {"type": "integer"},
        "reference": {
            "type": "object", "additionalProperties": False, "required": ["path"],
            "properties": {"path": {"type": "string"}, "page": {"type": "integer", "minimum": 1},
                           "box": {"enum": ["trim", "media", "crop", "art"]}},
        },
        "sample": {
            "type": "object", "additionalProperties": False, "required": ["path"],
            "properties": {"path": {"type": "string"}, "page": {"type": "integer", "minimum": 1},
                           "kind": {"enum": ["scan", "pdf", "image"]}, "dpi": {"type": "number"}},
        },
        "work_dpi": {"type": "number", "minimum": 150, "maximum": 2400},
        "instances": {
            "type": "object", "additionalProperties": False,
            "properties": {
                "mode": {"enum": ["single", "multi"]},
                "expected_layout": {"type": "object", "additionalProperties": False,
                                    "properties": {"rows": {"type": "integer"}, "cols": {"type": "integer"}}},
                "expected_count": {"type": "integer"},
                "rotations": {"type": "array", "items": {"enum": [0, 90, 180, 270]}},
                "max_skew_deg": {"type": "number"},
                "systematic_ratio": {"type": "number", "minimum": 0.0, "maximum": 1.0},
                "consensus": {"type": "boolean"},
            },
        },
        "masks": {"type": "array", "items": {
            "type": "object", "additionalProperties": False, "required": ["reason", "rect_mm"],
            "properties": {"reason": {"enum": ["variable", "user"]},
                           "rect_mm": {"type": "array", "items": {"type": "number"}, "minItems": 4, "maxItems": 4}}}},
        "spot_rules": {"type": "object", "additionalProperties": {"type": "string"}},
        "layers": {
            "type": "object", "additionalProperties": False,
            "properties": {
                "include": {"type": "array", "items": {"type": "string"}},
                "exclude": {"type": "array", "items": {"type": "string"}},
                "exclude_spots": {"type": "array", "items": {"type": "string"}},
            },
        },
        "text_compare": {"type": "boolean"},
        "color_calibrated": {"type": "boolean"},
        "tolerance_profile": {"type": "string"},
        "tolerance_overrides": {"type": "object"},
        "outputs": {
            "type": "object", "additionalProperties": False,
            "properties": {"dir": {"type": "string"}, "json": {"type": "string"},
                           "report_html": {"type": "string"}, "layers": {"type": "boolean"},
                           "layer_dpi": {"type": "number"}},
        },
    },
}

DEFAULTS = {
    "seed": 1,
    "work_dpi": 600,
    "instances": {"mode": "single", "rotations": [0, 90, 180, 270], "max_skew_deg": 4.0,
                  "systematic_ratio": 0.8, "consensus": True},
    "masks": [],
    "layers": {},
    "text_compare": True,
    "tolerance_profile": "label_standard",
    "tolerance_overrides": {},
    "outputs": {"dir": "out", "json": "result.json", "report_html": "report.html", "layers": True, "layer_dpi": 200},
}


def resolve_profile(name: str, overrides: dict | None = None) -> dict:
    if name not in PROFILES:
        raise ValueError(f"okänd toleransprofil: {name}")
    p = copy.deepcopy(PROFILES[name])
    chain = []
    while "extends" in p:
        parent = p.pop("extends")
        chain.append(p)
        p = copy.deepcopy(PROFILES[parent])
    for child in reversed(chain):
        p.update(child)
    unknown = set(overrides or {}) - set(p)
    if unknown:
        raise ValueError(f"okända toleransparametrar: {sorted(unknown)}")
    p.update(overrides or {})
    return p


def load(path: str | Path) -> dict:
    path = Path(path)
    spec = json.loads(path.read_text(encoding="utf-8"))
    return normalize(spec, base_dir=path.parent)


def normalize(spec: dict, base_dir: Path | None = None) -> dict:
    jsonschema.validate(spec, SCHEMA)
    out = copy.deepcopy(DEFAULTS)
    for k, v in spec.items():
        if isinstance(v, dict) and isinstance(out.get(k), dict):
            out[k].update(v)
        else:
            out[k] = v
    out["reference"].setdefault("page", 1)
    out["reference"].setdefault("box", "trim")
    out["sample"].setdefault("page", 1)
    if "kind" not in out["sample"]:
        out["sample"]["kind"] = "pdf" if out["sample"]["path"].lower().endswith(".pdf") else "scan"
    if out["sample"]["kind"] == "pdf" and "tolerance_profile" not in spec:
        out["tolerance_profile"] = "pdf_vs_pdf"
    base = Path(base_dir) if base_dir else Path.cwd()
    for key in ("reference", "sample"):
        p = Path(out[key]["path"])
        out[key]["path"] = str(p if p.is_absolute() else (base / p).resolve())
    od = Path(out["outputs"]["dir"])
    out["outputs"]["dir"] = str(od if od.is_absolute() else (base / od).resolve())
    out["tolerances"] = resolve_profile(out["tolerance_profile"], out["tolerance_overrides"])
    return out
