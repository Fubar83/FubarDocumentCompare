"""Kommandorad: python -m printcmp run jobspec.json [--out DIR]"""
from __future__ import annotations

import argparse
import json
import sys
import time

from . import jobspec, pipeline


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="printcmp", description="Grafisk jämförelse av tryck mot PDF-original")
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run", help="kör ett jobb")
    r.add_argument("jobspec")
    r.add_argument("--out", help="utdatakatalog (ersätter outputs.dir)")
    r.add_argument("--quiet", action="store_true")
    a = ap.parse_args(argv)

    if a.cmd == "run":
        spec = jobspec.load(a.jobspec)
        if a.out:
            from pathlib import Path
            spec["outputs"]["dir"] = str(Path(a.out).resolve())
        t0 = time.time()

        def progress(stage, frac):
            if not a.quiet:
                print(f"\r{stage:<10} {frac * 100:5.1f} %", end="", file=sys.stderr, flush=True)

        res = pipeline.run(spec, progress)
        if not a.quiet:
            print(file=sys.stderr)
            s = res["summary"]
            print(json.dumps({"instances": res["sheet"]["instances_found"], "differences": s["differences"],
                              "by_kind": s["by_kind"], "fail": s["instances_fail"], "seconds": round(time.time() - t0, 1),
                              "out": spec["outputs"]["dir"]}, ensure_ascii=False))
        return 0
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
