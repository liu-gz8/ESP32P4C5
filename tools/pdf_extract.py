"""Extract text from a PDF using the Codex bundled Python (pypdf).

Usage:
    python pdf_extract.py <pdf> [--pages N-M] [--grep PATTERN] [--out FILE]
"""

from __future__ import annotations

import argparse
import re
import sys

from pypdf import PdfReader


def parse_pages(spec: str, total: int) -> list[int]:
    if not spec:
        return list(range(total))
    m = re.fullmatch(r"(\d+)-(\d+)", spec.strip())
    if m:
        start, end = int(m.group(1)), int(m.group(2))
        return [p for p in range(start - 1, min(end, total))]
    return [int(spec) - 1]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("pdf")
    ap.add_argument("--pages", default="")
    ap.add_argument("--grep", default="")
    ap.add_argument("--out", default="")
    args = ap.parse_args()

    reader = PdfReader(args.pdf)
    total = len(reader.pages)
    print(f"[pdf] {args.pdf}: {total} pages", file=sys.stderr)

    chunks: list[str] = []
    pattern = re.compile(args.grep, re.IGNORECASE) if args.grep else None

    for index in parse_pages(args.pages, total):
        if index < 0 or index >= total:
            continue
        try:
            text = reader.pages[index].extract_text() or ""
        except Exception as exc:  # noqa: BLE001
            text = f"<<extract failed: {exc}>>"
        if pattern and not pattern.search(text):
            continue
        chunks.append(f"\n===== page {index + 1} =====\n{text}")

    out = "\n".join(chunks)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as handle:
            handle.write(out)
        print(f"[pdf] wrote {len(out)} chars to {args.out}", file=sys.stderr)
    else:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        print(out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
