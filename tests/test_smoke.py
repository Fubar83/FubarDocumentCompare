"""Snabba tester: python -m unittest discover -s tests -v"""
from __future__ import annotations

import copy
import json
import sys
import tempfile
import unittest
from pathlib import Path

import cv2
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from printcmp import barcode, braille, jobspec, objmap, pdfspots, pipeline, register, render, structure  # noqa: E402
from tools.testgen import labelpdf  # noqa: E402
from tools.testgen.templates import nutrition_grid, nutrition_open, oat_drink  # noqa: E402


def _analyse(spec, dpi=300):
    pdf = labelpdf.render_pdf(spec)
    sp = pdfspots.analyze(pdf)
    ref = render.PdfiumRenderer().render(sp.sanitized_pdf, 0, dpi)
    codes = barcode.detect(ref.rgb, dpi)
    objs = objmap.build(sp.sanitized_pdf, 0, ref.geom, [c.bbox for c in codes])
    return pdf, sp, ref, objs


class TestSpots(unittest.TestCase):
    def test_dieline_and_varnish_found_and_removed(self):
        pdf = labelpdf.render_pdf(oat_drink())
        a = pdfspots.analyze(pdf)
        self.assertTrue(a.polygons("dieline"), "stanslinje saknas")
        self.assertTrue(a.polygons("varnish"), "lack saknas")
        self.assertIn("CutContour", a.spot_names)
        self.assertIn("PANTONE 347 C", a.spot_names)
        self.assertNotEqual(len(a.sanitized_pdf), 0)


class TestBraille(unittest.TestCase):
    def test_roundtrip_swedish(self):
        for t in ["havredryck", "Fett 1,5 g", "åäö - salt."]:
            self.assertEqual(braille.cells_to_text(braille.text_to_cells(t)), t)

    def test_decode_from_pdf(self):
        pdf, sp, ref, _ = _analyse(nutrition_grid())
        lines = braille.from_pdf(sp, pdf, 0, ref.geom)
        self.assertEqual([ln.text for ln in lines], ["havredryck"])
        self.assertAlmostEqual(lines[0].pitch / ref.geom.px_per_mm(), 2.5, delta=0.05)
        self.assertAlmostEqual(lines[0].cell_pitch / ref.geom.px_per_mm(), 6.0, delta=0.05)


class TestStructure(unittest.TestCase):
    def test_ruled_and_unruled_tables(self):
        for spec, kind in ((nutrition_grid(), "ruled_grid"), (nutrition_open(), "unruled")):
            _, _, ref, objs = _analyse(spec)
            ts = structure.detect(objs, ref.geom.px_per_mm())
            self.assertEqual(len(ts), 1, spec["name"])
            t = ts[0]
            self.assertEqual(t.kind, kind)
            self.assertEqual((t.n_rows, t.n_cols), (8, 2))
            self.assertEqual(t.cells[(2, 1)], "1,5 g")
            self.assertEqual(t.cells[(3, 0)], "varav mättat fett")

    def test_no_table_in_paragraphs(self):
        _, _, ref, objs = _analyse(oat_drink())
        self.assertEqual(structure.detect(objs, ref.geom.px_per_mm()), [])

    def test_outlined_text(self):
        _, _, _, objs = _analyse(nutrition_grid())
        ot = [o for o in objs if o.kind == "OutlinedText"]
        self.assertEqual(len(ot), 1)
        self.assertEqual(len(ot[0].extra["glyphs"]), len("Havrebryggan"))


class TestColor(unittest.TestCase):
    def test_ciede2000_sharma_pair1(self):
        from skimage.color import deltaE_ciede2000
        d = deltaE_ciede2000(np.array([50.0, 2.6772, -79.7751]), np.array([50.0, 0.0, -82.7485]))
        self.assertAlmostEqual(float(d), 2.0425, places=3)


class TestRegistration(unittest.TestCase):
    def test_global_shift_recovered(self):
        rng = np.random.default_rng(0)
        ref = cv2.GaussianBlur(rng.random((600, 800)).astype(np.float32), (0, 0), 3)
        ref = (ref - ref.min()) / (ref.max() - ref.min())
        M = np.float32([[1, 0, 7.3], [0, 1, -4.6]])
        smp = cv2.warpAffine(ref, M, (900, 700), borderMode=cv2.BORDER_REFLECT)
        valid = np.ones(ref.shape, bool)
        H, rms, n = register.refine_global(ref, smp, np.eye(3), valid, tile=128)
        self.assertGreater(n, 10)
        self.assertAlmostEqual(H[0, 2], 7.3, delta=0.15)
        self.assertAlmostEqual(H[1, 2], -4.6, delta=0.15)


class TestPdfVsPdf(unittest.TestCase):
    def _run(self, ref_spec, smp_spec):
        tmp = Path(tempfile.mkdtemp())
        (tmp / "ref.pdf").write_bytes(labelpdf.render_pdf(ref_spec))
        (tmp / "smp.pdf").write_bytes(labelpdf.render_pdf(smp_spec))
        spec = jobspec.normalize({"schema": jobspec.SCHEMA_ID, "reference": {"path": "ref.pdf"},
                                  "sample": {"path": "smp.pdf"}, "work_dpi": 300,
                                  "outputs": {"dir": "out", "layers": False, "report_html": ""}}, base_dir=tmp)
        return pipeline.run(spec)

    def test_identical_has_no_differences(self):
        s = oat_drink()
        res = self._run(s, s)
        self.assertEqual([d for d in res["differences"] if d["kind"] != "Uncertain"], [])

    def test_changed_character_is_found(self):
        s = oat_drink()
        m = copy.deepcopy(s)
        el = next(e for e in m["elements"] if e["id"] == "ingr1")
        el["text"] = el["text"].replace("10 %", "18 %")
        res = self._run(s, m)
        hits = [d for d in res["differences"] if d["kind"] != "Uncertain"]
        self.assertTrue(hits, "ändringen hittades inte")
        texts = " ".join(o.get("text", "") for d in hits for o in d["objects"])
        self.assertIn("10", texts)


class TestCategoriesPdfVsPdf(TestPdfVsPdf):
    def test_table_outline_braille_changes(self):
        a = nutrition_grid()
        b = copy.deepcopy(a)
        t = next(e for e in b["elements"] if e["id"] == "nut")
        t["rows"][3][1] = "0,7 g"
        t["skip_rules"] = [5]
        next(e for e in b["elements"] if e["id"] == "brand")["text"] = "Havrebrygqan"
        next(e for e in b["elements"] if e["id"] == "braille")["text"] = "havredrvck"
        res = self._run(a, b)
        got = {(d["kind"], d.get("category")) for d in res["differences"]}
        self.assertIn(("Changed", "TableCell"), got)
        self.assertIn(("Missing", "TableRule"), got)
        self.assertIn(("Changed", "OutlinedText"), got)
        self.assertIn(("Braille", "Braille"), got)
        cell = next(d for d in res["differences"] if d.get("category") == "TableCell")
        self.assertEqual((cell["table"]["row"], cell["table"]["col"]), (3, 1))
        rule = next(d for d in res["differences"] if d.get("category") == "TableRule")
        self.assertEqual(rule["table"]["between_rows"], [4, 5])
        br = next(d for d in res["differences"] if d["kind"] == "Braille")
        self.assertEqual((br["braille"]["expected"], br["braille"]["read"]), ("y", "v"))
        self.assertEqual(len(res["differences"]), 4)


if __name__ == "__main__":
    unittest.main()
