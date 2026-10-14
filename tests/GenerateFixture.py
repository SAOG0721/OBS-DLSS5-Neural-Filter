"""Create a deterministic SDR transparency pattern for the actual-OBS harness."""
from pathlib import Path
import sys
import numpy as np
from PIL import Image
width, height = (int(sys.argv[2]), int(sys.argv[3])) if len(sys.argv)>3 else (1280, 720)
y, x = np.mgrid[:height, :width]
pixels = np.empty((height, width, 4), dtype=np.uint8)
pixels[..., 0] = (x * 3 + y * 2) % 256
pixels[..., 1] = (x * 2 + y * 5) % 256
pixels[..., 2] = (x * 5 + y * 3) % 256
pixels[..., 3] = np.array([0, 64, 128, 255], dtype=np.uint8)[np.minimum(x * 4 // width, 3)]
output = Path(sys.argv[1])
output.parent.mkdir(parents=True, exist_ok=True)
Image.fromarray(pixels).save(output)
print(output.resolve())
