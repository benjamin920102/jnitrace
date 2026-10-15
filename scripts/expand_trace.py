#!/usr/bin/env python3
"""Expand context references in a v2 JSONL trace; supports .gz input/output."""
import argparse
import gzip
import json
from pathlib import Path


def open_text(path, mode):
    return (gzip.open if str(path).endswith(".gz") else open)(path, mode, encoding="utf-8")


def expand(records):
    contexts = {}
    for record in records:
        if record.get("ev") == "context":
            key = record["id"]
            if key in contexts:
                raise ValueError(f"Duplicate context {key}")
            contexts[key] = {k: v for k, v in record.items() if k not in ("ev", "id")}
            continue
        record = dict(record)
        key = record.pop("ctx", None)
        if key is not None:
            if key not in contexts:
                raise ValueError(f"Missing context {key}; use the complete trace")
            record = {**contexts[key], **record}
        yield record


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("input", type=Path)
    p.add_argument("output", type=Path)
    a = p.parse_args()
    if a.input.resolve() == a.output.resolve():
        p.error("input and output must differ")
    with open_text(a.input, "rt") as src, open_text(a.output, "wt") as dst:
        for record in expand(json.loads(line) for line in src if line.strip()):
            dst.write(json.dumps(record, ensure_ascii=True, separators=(",", ":"), allow_nan=False) + "\n")


if __name__ == "__main__":
    main()
