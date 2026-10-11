#!/usr/bin/env python3
"""Generate and build the PrismCC class example with the host compiler."""

from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "examples" / "classes_prismcc.c"
OUTPUT = ROOT / "build" / "classes.app"


def main() -> int:
    print(f"Source: {SOURCE}")
    print(f"Output: {OUTPUT}")

    result = subprocess.run(
        ["make", "prismcc"],
        cwd=ROOT,
        check=False,
    )
    if result.returncode != 0:
        print("Failed to build the standalone PrismCC compiler.", file=sys.stderr)
        return result.returncode

    result = subprocess.run(
        [str(ROOT / "build" / "prismcc"), str(SOURCE), str(OUTPUT)],
        cwd=ROOT,
        check=False,
    )
    if result.returncode == 0:
        print("Class example compiled successfully.")
        return 0

    print(
        "The host PrismCC compiler does not support class syntax yet. "
        "Compile this source with PrismOS's in-OS `cc` command.",
        file=sys.stderr,
    )
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
