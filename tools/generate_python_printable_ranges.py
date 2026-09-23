#!/usr/bin/env python3
"""Generate the pinned Unicode 15 ranges used by Python 3.12 str repr.

The input is the Unicode 15.0.0 UnicodeData.txt source file. The generated
header is data only; product execution never invokes Python or UnicodeData.
"""

from __future__ import annotations

import hashlib
from pathlib import Path
import sys


EXPECTED_SHA256 = "806e9aed65037197f1ec85e12be6e8cd870fc5608b4de0fffd990f689f376a73"
OUTPUT = Path(__file__).resolve().parents[1] / "cpp" / "python_printable_ranges.hpp"


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: generate_python_printable_ranges.py UnicodeData.txt")
    source = Path(sys.argv[1]).read_bytes()
    if hashlib.sha256(source).hexdigest() != EXPECTED_SHA256:
        raise RuntimeError("UnicodeData 15.0.0 digest mismatch")
    categories: dict[int, str] = {}
    range_start: tuple[int, str] | None = None
    for raw in source.decode("utf-8").splitlines():
        fields = raw.split(";")
        point, name, category = int(fields[0], 16), fields[1], fields[2]
        if name.endswith(", First>"):
            range_start = (point, category)
        elif name.endswith(", Last>"):
            if range_start is None or range_start[1] != category:
                raise RuntimeError("UnicodeData range mismatch")
            for value in range(range_start[0], point + 1):
                categories[value] = category
            range_start = None
        else:
            categories[point] = category
    if range_start is not None:
        raise RuntimeError("unterminated UnicodeData range")

    ranges: list[tuple[int, int]] = []
    first: int | None = None
    for point in range(0x110000):
        category = categories.get(point, "Cn")
        printable = point == 0x20 or category[0] not in {"C", "Z"}
        if printable and first is None:
            first = point
        elif not printable and first is not None:
            ranges.append((first, point - 1))
            first = None
    if first is not None:
        ranges.append((first, 0x10FFFF))

    lines = [
        "// Generated from UnicodeData 15.0.0 by tools/generate_python_printable_ranges.py",
        f"// UnicodeData SHA-256: {EXPECTED_SHA256}",
        "// Python 3.12 str repr treats category Other/Separator as nonprintable, except U+0020.",
        "#pragma once",
        "#include <cstdint>",
        "namespace swegca::vrs::unicode_table {",
        "struct PrintableRange { std::uint32_t first; std::uint32_t last; };",
        "inline constexpr PrintableRange printable_ranges[] = {",
    ]
    lines.extend(f"    {{0x{start:x}u, 0x{end:x}u}}," for start, end in ranges)
    lines.extend(["};", "}  // namespace swegca::vrs::unicode_table", ""])
    OUTPUT.write_text("\n".join(lines), encoding="utf-8")
    print(f"generated {OUTPUT}: {len(ranges)} printable ranges")


if __name__ == "__main__":
    main()
