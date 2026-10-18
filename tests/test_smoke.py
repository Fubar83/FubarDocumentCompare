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

from printcmp import jobspec, pdfspots, pipeline, register  # noqa: E402
from tools.testgen import labelpdf  # noqa: E402
from tools.testgen.templates import oat_drink  # noqa: E402


class TestSpots(unittest.TestCase):
    def test_dieline_and_varnish_found_and_removed(self):
        pdf = labelpdf.render_pdf(oat_drink())
        a = pdfspots.analyze(pdf)
        self.assertTrue(a.polygons("dieline"), "stanslinje saknas")
        self.assertTrue(a.polygons("varnish"), "lack saknas")
        self.assertIn("CutContour", a.spot_names)
        self.assertIn("PANTONE 347 C", a.spot_names)
        self.assertNotEqual(len(a.sanitized_pdf), 0)


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


if __name__ == "__main__":
    unittest.main()
