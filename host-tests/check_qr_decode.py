"""Decode fixtures from build_oled.cmd; requires Pillow and zxing-cpp."""
from pathlib import Path

from PIL import Image
import zxingcpp

root = Path(__file__).resolve().parents[1] / "build/registration-check"
identities = (root / "ids.txt").read_text().splitlines()
passed = 0
for index, identity in enumerate(identities):
    for phase in range(9):
        with Image.open(root / f"qr_{index}_{phase}.pgm") as source:
            # Simulate screen pixels resolved by a camera; no interpolation.
            enlarged = source.resize((1024, 512), Image.Resampling.NEAREST)
            for angle in (0, 90):
                image = enlarged.rotate(angle, expand=True)
                result = zxingcpp.read_barcode(image)
                if result is None or result.text != identity:
                    raise RuntimeError(f"Decode failed: fixture={index}, phase={phase}, angle={angle}")
                passed += 1
print(f"PASS: {passed} full-ID QR decodes across 32 IDs, 9 shifts and 2 orientations")
