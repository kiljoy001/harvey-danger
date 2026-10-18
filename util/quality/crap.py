#!/usr/bin/env python3
"""Calculate per-function CRAP scores from LLVM coverage exports."""

import argparse
import json
from pathlib import Path
import sys


def function_source(function):
    filenames = function.get("filenames", [])
    return str(Path(filenames[0]).resolve()) if filenames else ""


def score(function):
    branches = function.get("branches", [])
    complexity = 1 + len(branches)
    if branches:
        outcomes = 2 * len(branches)
        covered = sum(int(branch[4] > 0) + int(branch[5] > 0)
                      for branch in branches)
        coverage = covered / outcomes
    else:
        coverage = 1.0 if function.get("count", 0) > 0 else 0.0
    value = complexity * complexity * (1.0 - coverage) ** 3 + complexity
    return complexity, coverage, value


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("exports", nargs="+")
    parser.add_argument("--source", action="append", required=True)
    parser.add_argument("--maximum", type=float, default=50.0)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    sources = {str(Path(source).resolve()) for source in args.source}
    rows = []
    for export in args.exports:
        document = json.loads(Path(export).read_text())
        for unit in document.get("data", []):
            for function in unit.get("functions", []):
                source = function_source(function)
                if source not in sources:
                    continue
                complexity, coverage, value = score(function)
                name = function["name"].rsplit(":", 1)[-1]
                rows.append({
                    "source": source,
                    "function": name,
                    "complexity": complexity,
                    "coverage": round(coverage * 100.0, 2),
                    "crap": round(value, 2),
                })

    if not rows:
        print("crap: no functions matched the requested sources", file=sys.stderr)
        return 2
    rows.sort(key=lambda row: (-row["crap"], row["function"]))
    Path(args.output).write_text(json.dumps({
        "formula": "complexity^2 * (1 - coverage)^3 + complexity",
        "coverage": "LLVM branch outcomes",
        "maximum": args.maximum,
        "functions": rows,
    }, indent=2) + "\n")

    print("quality: CRAP function                              CC   cover   score")
    for row in rows:
        print(f"quality: CRAP {row['function'][:36]:36} "
              f"{row['complexity']:3d} {row['coverage']:6.2f}% "
              f"{row['crap']:7.2f}")
    worst = rows[0]
    if worst["crap"] > args.maximum:
        print(f"quality: CRAP limit {args.maximum:.2f} exceeded by "
              f"{worst['function']} ({worst['crap']:.2f})", file=sys.stderr)
        return 1
    print(f"quality: CRAP PASS maximum {worst['crap']:.2f} "
          f"<= {args.maximum:.2f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
