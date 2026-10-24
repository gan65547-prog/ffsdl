#!/usr/bin/env python3
"""Check the ARM64 runtime files that a fresh Git checkout will contain."""

import pathlib
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
PREFIX = pathlib.Path("third_party/lib/aarch64-linux-gnu")
REQUIRED = (
    "libavcodec.so.58",
    "libavformat.so.58",
    "libavutil.so.56",
    "libswscale.so.5",
    "libswresample.so.3",
    "libSDL2-2.0.so.0",
    "librockchip_mpp.so",
    "librga.so",
    "libLLVM-14.so.1",
    "dri/rockchip_dri.so",
)


def main() -> int:
    tracked = subprocess.check_output(
        ["git", "ls-files", "-z", "--", str(PREFIX)], cwd=ROOT
    ).decode().split("\0")
    tracked = {pathlib.Path(name) for name in tracked if name}
    errors = []

    for name in REQUIRED:
        path = PREFIX / name
        if path not in tracked:
            errors.append(f"not tracked: {path}")

    for relative in sorted(tracked):
        path = ROOT / relative
        if not path.exists():
            errors.append(f"missing file or broken link: {relative}")
            continue
        if path.is_symlink():
            continue
        with path.open("rb") as stream:
            header = stream.read(64)
        if header.startswith(b"version https://git-lfs.github.com/spec/v1"):
            errors.append(f"Git LFS object not downloaded: {relative}")
        elif header[:4] != b"\x7fELF" or header[4] != 2 or header[18:20] != b"\xb7\x00":
            errors.append(f"not a 64-bit AArch64 ELF library: {relative}")

    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print(f"OK: {len(tracked)} tracked ARM64 runtime files and links")
    return 0


if __name__ == "__main__":
    sys.exit(main())
