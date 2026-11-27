"""Prepare isolated DLSSNR 0.6.12 host cases, or inspect moving OBS/NVENC recordings.

Requires NumPy, Pillow and PyAV in the selected Python environment. An existing
project-local .deps/test-python installation of PyAV is also supported. All
default content is procedural; a photo is used only through explicit --photo.
"""
from __future__ import annotations

import argparse
import copy
from fractions import Fraction
import json
from pathlib import Path
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw

PROJECT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT / ".deps" / "test-python"))


def prepare(destination: Path, seconds: int, photo: Path | None = None) -> None:
    import av

    destination = destination.resolve()
    destination.mkdir(parents=True, exist_ok=True)
    settings_directory = destination / "settings"
    settings_directory.mkdir(exist_ok=True)
    subprocess.run(
        [sys.executable, str(PROJECT / "tests" / "GenerateFixture.py"),
         str(destination / "alpha-1280x720.png"), "1280", "720"],
        check=True,
    )
    base = {
        "schemaVersion": 2, "model_preset": 1,
        "scaling": False, "resolution_control": 1, "percent": 100,
        "processing_mode": 0, "update_interval": 2, "residual_transfer": 0,
        "midtone": 1.0, "chroma_protection": 0.0,
        "passes": 1, "advanced": False, "p2_style": 2, "p3_intensity": 0.65,
        "flow": 0, "amd_quality": 1, "nv_quality": 2,
        "af": 0, "af_strength": 0.75, "response_ms": 80.0,
        "future_extension": "preserve this unknown field",
        "runtime": "C:/this-setting-must-be-overridden-by-harness-argv4.dll",
    }
    cases = {"ht-manual": copy.deepcopy(base)}
    cases["legacy-scene"] = {
        "schemaVersion": 1, "passes": 1, "scaling": True, "percent": 73,
        "flow": 0, "amd_quality": 0, "nv_quality": 4,
        "af": 3, "af_strength": 0.7, "response_ms": 500.0,
        "p2_style": 2, "p3_intensity": 0.65,
        "future_extension": "legacy unknown field must survive",
    }
    for name, overrides in {
        "auto-model": {"model_preset": 0},
        "fp8-model": {"model_preset": 2},
        "color-controls": {"midtone": 1.6, "chroma_protection": 0.9},
        "adaptive": {"scaling": True, "resolution_control": 0, "percent": 61},
        "alternating-2": {"processing_mode": 1, "update_interval": 2},
        "alternating-3": {"processing_mode": 1, "update_interval": 3},
        "alternating-4": {"processing_mode": 1, "update_interval": 4},
        "alternating-amd": {"processing_mode": 1, "update_interval": 3,
                            "residual_transfer": 1, "flow": 1},
        "alternating-nv": {"processing_mode": 1, "update_interval": 4,
                           "residual_transfer": 1, "flow": 2},
        "reproject-no-nr-flow-af": {"processing_mode": 1, "update_interval": 2,
                                   "residual_transfer": 1, "flow": 0,
                                   "af": 3, "response_ms": 10.0},
    }.items():
        cases[name] = dict(copy.deepcopy(base), **overrides)
    for name, settings in cases.items():
        (settings_directory / f"{name}.json").write_text(
            json.dumps(settings, indent=2) + "\n", encoding="utf-8")

    width, height = 1280, 720
    if photo is not None:
        if not photo.is_file():
            raise FileNotFoundError(f"Requested --photo does not exist: {photo}")
        with Image.open(photo) as image:
            background = image.convert("RGB").resize((width + 160, height + 90), Image.Resampling.LANCZOS)
        image_origin = "explicit --photo image with deterministic pan and overlays"
    else:
        y, x = np.mgrid[:height + 90, :width + 160]
        rgb = np.stack(((x * 3 + y * 2) % 256,
                        (x * 2 + y * 5) % 256,
                        (x * 5 + y * 3) % 256), axis=-1).astype(np.uint8)
        background = Image.fromarray(rgb)
        image_origin = "deterministic synthetic color/texture pattern"

    media = []
    for fps in (30, 60):
        path = destination / f"moving-1280x720-{fps}fps.mp4"
        with av.open(str(path), mode="w") as container:
            stream = container.add_stream("libx264", rate=fps)
            stream.width, stream.height = width, height
            stream.pix_fmt = "yuv420p"
            stream.options = {"preset": "veryfast", "crf": "16"}
            stream.time_base = Fraction(1, fps)
            for index in range(fps * seconds):
                time = index / fps
                left = int(80 + 65 * np.sin(time * 0.8))
                top = int(45 + 35 * np.sin(time * 0.55))
                image = background.crop((left, top, left + width, top + height))
                draw = ImageDraw.Draw(image)
                box_x = int((time * 180) % (width + 100)) - 100
                draw.rectangle((box_x, 420, box_x + 100, 580), fill=(240, 80, 40), outline=(255, 255, 255), width=3)
                bar_x = int((time * 280) % width)
                draw.line((bar_x, 0, bar_x, height - 1), fill=(40, 240, 120), width=5)
                draw.rectangle((12, 12, 390, 45), fill=(12, 12, 12))
                draw.text((20, 20), f"DLSSNR host motion: {fps} FPS / frame {index:04d}", fill=(255, 255, 255))
                frame = av.VideoFrame.from_image(image)
                frame.pts, frame.time_base = index, Fraction(1, fps)
                for packet in stream.encode(frame):
                    container.mux(packet)
            for packet in stream.encode():
                container.mux(packet)
        # Check the produced asset's timing without opening any OBS session.
        with av.open(str(path)) as container:
            stream = container.streams.video[0]
            assert stream.average_rate == fps and stream.width == width and stream.height == height
            media.append({"file": path.name, "fps": fps, "frames": stream.frames,
                          "width": width, "height": height, "seconds": seconds})
    manifest = {
        "scope": "Isolated libobs source/filter and short NVENC checks; synthetic motion is not gameplay or visual acceptance.",
        "canvasFps": 60, "mediaOrigin": image_origin,
        "alphaFixture": "alpha-1280x720.png", "media": media,
        "settingsCases": [f"settings/{name}.json" for name in cases],
        "runtimeOverride": "ObsHarness argv[4] always replaces any runtime value in argv[8] settings JSON.",
        "mediaModes": "--media checks moving playback, then pauses for the existing exact-pixel contracts. --media-record additionally restarts playback for each 5-second NVENC recording.",
    }
    (destination / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"fixtureDirectory": str(destination), "settingsCases": len(cases), "media": media}, indent=2))


def verify_recordings(directory: Path) -> None:
    import av

    directory = directory.resolve()
    expected_width, expected_height = map(int, (directory / "dimensions.txt").read_text().split())
    report = {"scope": "Moving ffmpeg_source through actual libobs and short NVENC recordings; correction/alpha checks come from the paused-source harness contracts.",
              "recordings": {}}
    for name in ("original", "neural"):
        decoded = 0
        changing = 0
        previous_pixels = None
        previous_pts = None
        with av.open(str(directory / f"{name}.mkv")) as container:
            stream = container.streams.video[0]
            assert (stream.width, stream.height) == (expected_width, expected_height), "Recorded dimensions changed"
            for frame in container.decode(video=0):
                assert frame.pts is not None and (previous_pts is None or frame.pts > previous_pts), "Non-monotonic video timestamps"
                pixels = frame.to_ndarray(format="rgb24")
                if previous_pixels is not None and np.any(pixels != previous_pixels):
                    changing += 1
                if decoded == 60:
                    Image.fromarray(pixels).save(directory / f"{name}-moving-decoded.png")
                previous_pixels, previous_pts = pixels, frame.pts
                decoded += 1
            assert decoded >= 250, "Recording is shorter than the existing 5-second host check"
            assert changing >= 60, "Moving-source recording remained frozen"
            report["recordings"][name] = {
                "codec": stream.codec_context.name, "rate": str(stream.average_rate),
                "decodedFrames": decoded, "changingFrames": changing,
                "timestampsMonotonic": True, "width": stream.width, "height": stream.height,
            }
    (directory / "media-recording-validation.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    prepare_parser = commands.add_parser("prepare")
    prepare_parser.add_argument("--output", required=True, type=Path)
    prepare_parser.add_argument("--seconds", type=int, default=8)
    prepare_parser.add_argument("--photo", type=Path, help="Optional image; defaults to a procedural texture")
    verify_parser = commands.add_parser("verify-recordings")
    verify_parser.add_argument("directory", type=Path)
    arguments = parser.parse_args()
    if arguments.command == "prepare":
        if arguments.seconds < 6:
            parser.error("--seconds must be at least 6 for a five-second recording")
        prepare(arguments.output, arguments.seconds, arguments.photo)
    else:
        verify_recordings(arguments.directory)
