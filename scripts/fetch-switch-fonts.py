#!/usr/bin/env python3
"""Fetch and verify the multilingual fonts introduced by upstream v0.6.5."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import sys
import tempfile
from urllib.request import urlopen


UPSTREAM_REVISION = "6a8087311b421ddee8017f163a602a0305a56c03"
FONT_HASHES = {
    "NotoSansArabic-Regular.ttf": "ceea25b464a656dc3b26849bab9356740401af62aedf1bfa8b7f0d9b75925b1b",
    "NotoSansCJKjp-VF.ttf": "240c9b83bf7b386edbae39995ae7e068ed4583f484d92e4a74c34158b5f27b1a",
    "NotoSansHebrew-Regular.ttf": "a7fa16fffb27bedb060a0866267c29e9859aeb9c21cc33f5b3aaf6eb062eca85",
    "NotoSansLao-Regular.ttf": "4a64d40850990992913d4be44b2e87a93d52f76882c1f0c0755edcb12ec57aa2",
    "NotoSansThai-Regular.ttf": "404ddfb5ed0aaa6b6ec8a85700d682978992062d67da93903967b56cbd9a4acc",
}
BASE_URL = (
    "https://raw.githubusercontent.com/SirSamael/"
    f"ppsspp-switch-community-build/{UPSTREAM_REVISION}/assets"
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def fetch_font(destination: Path, expected_hash: str) -> None:
    if destination.is_file() and sha256(destination) == expected_hash:
        print(f"Verified {destination.name}")
        return

    destination.parent.mkdir(parents=True, exist_ok=True)
    url = f"{BASE_URL}/{destination.name}"
    print(f"Downloading {destination.name} from upstream v0.6.5...")

    fd, temporary_name = tempfile.mkstemp(
        prefix=f".{destination.name}.", dir=destination.parent
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(fd, "wb") as output, urlopen(url, timeout=120) as response:
            while chunk := response.read(1024 * 1024):
                output.write(chunk)

        actual_hash = sha256(temporary)
        if actual_hash != expected_hash:
            raise RuntimeError(
                f"SHA-256 mismatch for {destination.name}: "
                f"expected {expected_hash}, got {actual_hash}"
            )
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    assets = root / "assets"
    try:
        for filename, expected_hash in FONT_HASHES.items():
            fetch_font(assets / filename, expected_hash)
    except (OSError, RuntimeError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
