#!/usr/bin/env python3
"""Build and publish deterministic, privacy-safe media from release-HIL frames.

The release HIL captures raw framebuffer evidence locally.  This tool turns an
explicit subset into stable promotional assets, verifies their GIF structure
without relying on an external encoder, and stages only redacted text evidence
plus the safe media for artifact upload.  Raw PNG/RGB332 captures are never
copied into the public evidence tree.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Iterable


SCHEMA = 1
SOURCE_NAMES = [f"a{index:02d}" for index in range(1, 20)]
SAFE_MEDIA = ("hero.gif", "demo.gif", "hero.png", "manifest.json")
SAFE_TEXT_SUFFIXES = {".json", ".xml", ".log", ".txt"}
SAFE_PART_RE = re.compile(r"^[A-Za-z0-9._-]+$")
MAX_TEXT_FILE_BYTES = 2_000_000
MAX_TEXT_TOTAL_BYTES = 16_000_000
USB_RE = re.compile(r"(?i)\b(?:[0-9a-f]{2}:){5}[0-9a-f]{2}\b")
SERIAL_RE = re.compile(r"(?i)(?:/dev/(?:cu|tty)\.[^\s\"']+|\bCOM\d+\b)")
IPV4_RE = re.compile(
    r"(?<![0-9.])(?:"
    r"(?:10|127)(?:\.[0-9]{1,3}){3}|"
    r"(?:169\.254|172\.(?:1[6-9]|2[0-9]|3[01])|192\.168)(?:\.[0-9]{1,3}){2}"
    r")(?![0-9.])"
)
NODE_RE = re.compile(r"(?i)![0-9a-f]{8}\b")
# HIL replies are persisted both as their original ``key=value`` records and as
# JSON dictionaries.  Keep the key list aligned with every protocol field that
# can contain a node or message identifier; production peers can otherwise
# leak through a read-only navigation state even when the DUT id is protected.
NODE_FIELD_RE = re.compile(
    r"(?i)([\"']?\b(?:node|my|selected|last|id|from|to|reply|tx_reply|react_last_target)\b"
    r"[\"']?\s*(?:=|:)\s*[\"']?)!?[0-9a-f]{8}\b"
)
IPV6_RE = re.compile(
    r"(?i)(?<![0-9a-f:])(?:"
    r"(?:fc|fd)[0-9a-f]{2}(?::[0-9a-f]{0,4}){1,7}|"
    r"fe[89ab][0-9a-f](?::[0-9a-f]{0,4}){1,7}|::1"
    r")(?:%[A-Za-z0-9_.-]+)?(?![0-9a-f:])"
)
USER_PATH_RE = re.compile(r"/(?:Users|home)/[^/\s\"']+(?:/[^\s\"']*)?")
# macOS places each user's temporary files under a stable, user-scoped
# /var/folders token.  Even though the files themselves are ephemeral, that
# token fingerprints the runner account and must not enter public HIL logs.
# Redact ordinary Unix temp roots too so generated venv/work paths never become
# part of the public artifact contract.
TEMP_PATH_RE = re.compile(
    r"/(?:"
    r"(?:private/)?var/folders/[A-Za-z0-9]{2}/[A-Za-z0-9_-]+/[CT]"
    r"|(?:private/)?tmp"
    r")(?:/[^\s\"']*)?"
)
TOKEN_ALLOWLIST = {
    "access", "devices", "expected_region", "expected_tx_power", "host", "kind",
    "name", "port", "protected_devices", "read-only", "region", "schema",
    "test", "tx_power", "usb_serial", "wifi_peers",
}


class MediaError(ValueError):
    """A fail-closed media or evidence validation error."""


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def strict_object(value: object, keys: set[str], label: str) -> dict[str, object]:
    if not isinstance(value, dict) or set(value) != keys:
        actual = sorted(value) if isinstance(value, dict) else type(value).__name__
        raise MediaError(f"{label} must contain exactly {sorted(keys)!r}, got {actual!r}")
    return value


def positive_int(value: object, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise MediaError(f"{label} must be a positive integer")
    return value


def load_story(path: Path) -> dict[str, object]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise MediaError(f"cannot read story {path}: {exc}") from exc
    story = strict_object(
        value,
        {"schema", "source", "frames", "outputs", "poster_frame", "max_duration_ms"},
        "story",
    )
    if story["schema"] != SCHEMA:
        raise MediaError(f"unsupported story schema: {story['schema']!r}")

    source = strict_object(story["source"], {"width", "height"}, "source")
    source_width = positive_int(source["width"], "source.width")
    source_height = positive_int(source["height"], "source.height")
    if (source_width, source_height) != (720, 405):
        raise MediaError("release-HIL PNG geometry must be exactly 720x405")

    frames = story["frames"]
    if not isinstance(frames, list):
        raise MediaError("frames must be an array")
    parsed_frames: list[dict[str, object]] = []
    for index, item in enumerate(frames):
        frame = strict_object(item, {"name", "label", "delay_ms"}, f"frames[{index}]")
        name = frame["name"]
        label = frame["label"]
        delay = positive_int(frame["delay_ms"], f"frames[{index}].delay_ms")
        if not isinstance(name, str) or not re.fullmatch(r"a[0-9]{2}", name):
            raise MediaError(f"unsafe frame name: {name!r}")
        if not isinstance(label, str) or not re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", label):
            raise MediaError(f"unsafe or missing frame label: {label!r}")
        if delay < 100 or delay > 3000 or delay % 10:
            raise MediaError(f"{name} delay must be 100..3000 ms in 10 ms steps")
        parsed_frames.append({"name": name, "label": label, "delay_ms": delay})
    names = [str(item["name"]) for item in parsed_frames]
    if names != SOURCE_NAMES:
        raise MediaError(f"story frame set/order must be exactly {SOURCE_NAMES!r}, got {names!r}")

    duration = sum(int(item["delay_ms"]) for item in parsed_frames)
    maximum = positive_int(story["max_duration_ms"], "max_duration_ms")
    if maximum > 20_000 or duration > maximum:
        raise MediaError(f"story duration {duration} ms exceeds maximum {maximum} ms")
    if story["poster_frame"] not in names:
        raise MediaError("poster_frame must name one of the exact story frames")

    outputs = strict_object(story["outputs"], {"hero", "screen", "poster"}, "outputs")
    expected_files = {"hero": "hero.gif", "screen": "demo.gif", "poster": "hero.png"}
    for key, filename in expected_files.items():
        output = strict_object(outputs[key], {"filename", "width", "height", "max_bytes"}, f"outputs.{key}")
        if output["filename"] != filename:
            raise MediaError(f"outputs.{key}.filename must be {filename!r}")
        width = positive_int(output["width"], f"outputs.{key}.width")
        height = positive_int(output["height"], f"outputs.{key}.height")
        limit = positive_int(output["max_bytes"], f"outputs.{key}.max_bytes")
        if (key, width, height) not in {
            ("hero", 900, 600), ("screen", 960, 540), ("poster", 900, 600)
        }:
            raise MediaError(f"unexpected outputs.{key} geometry: {width}x{height}")
        if limit > 4_000_000:
            raise MediaError(f"outputs.{key}.max_bytes exceeds the 4 MB publication ceiling")
    return story


def load_mockup(path: Path, photo_override: Path | None = None) -> tuple[dict[str, object], Path]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise MediaError(f"cannot read mockup config {path}: {exc}") from exc
    cfg = strict_object(value, {"photo", "corners"}, "mockup config")
    if not isinstance(cfg["photo"], str) or Path(cfg["photo"]).name != cfg["photo"]:
        raise MediaError("mockup photo must be one safe sibling filename")
    corners = cfg["corners"]
    if (
        not isinstance(corners, list) or len(corners) != 4
        or any(not isinstance(pair, list) or len(pair) != 2 for pair in corners)
        or any(isinstance(value, bool) or not isinstance(value, int) for pair in corners for value in pair)
    ):
        raise MediaError("mockup corners must be four integer [x,y] pairs")
    photo = photo_override if photo_override is not None else path.parent / str(cfg["photo"])
    if not photo.is_file() or photo.is_symlink():
        raise MediaError(f"mockup photo is missing or unsafe: {photo}")
    return cfg, photo


def solve8(a: list[list[float]], b: list[float]) -> list[float]:
    matrix = [row[:] + [b[index]] for index, row in enumerate(a)]
    for column in range(8):
        pivot = max(range(column, 8), key=lambda row: abs(matrix[row][column]))
        if abs(matrix[pivot][column]) < 1e-12:
            raise MediaError("degenerate mockup screen quad")
        matrix[column], matrix[pivot] = matrix[pivot], matrix[column]
        divisor = matrix[column][column]
        matrix[column] = [value / divisor for value in matrix[column]]
        for row in range(8):
            if row != column and matrix[row][column]:
                factor = matrix[row][column]
                matrix[row] = [value - factor * other for value, other in zip(matrix[row], matrix[column])]
    return [matrix[index][8] for index in range(8)]


def perspective_coeffs(quad: list[tuple[float, float]], size: tuple[int, int]) -> list[float]:
    width, height = size
    rows: list[list[float]] = []
    rhs: list[float] = []
    for (out_x, out_y), (src_x, src_y) in zip(quad, ((0, 0), (width, 0), (width, height), (0, height))):
        rows.append([out_x, out_y, 1, 0, 0, 0, -src_x * out_x, -src_x * out_y])
        rhs.append(src_x)
        rows.append([0, 0, 0, out_x, out_y, 1, -src_y * out_x, -src_y * out_y])
        rhs.append(src_y)
    return solve8(rows, rhs)


def composite(photo, frame, corners: list[list[int]], width: int):
    from PIL import Image

    factor = width / photo.width
    canvas = photo.convert("RGB").resize((width, round(photo.height * factor)), Image.Resampling.LANCZOS)
    quad = [(x * factor, y * factor) for x, y in corners]
    pixels = frame.convert("RGB").resize((240, 135), Image.Resampling.NEAREST)
    pixels = pixels.resize((720, 405), Image.Resampling.NEAREST).convert("RGBA")
    coeffs = perspective_coeffs(quad, pixels.size)
    layer = pixels.transform(canvas.size, Image.Transform.PERSPECTIVE, coeffs, Image.Resampling.BICUBIC)
    mask = Image.new("L", pixels.size, 255).transform(
        canvas.size, Image.Transform.PERSPECTIVE, coeffs, Image.Resampling.BICUBIC
    )
    canvas.paste(layer, (0, 0), mask)
    return canvas


def rgb332_palette() -> list[int]:
    palette: list[int] = []
    for value in range(256):
        palette.extend(
            ((value >> 5) * 255 // 7, ((value >> 2) & 7) * 255 // 7, (value & 3) * 255 // 3)
        )
    return palette


def gif_info(path: Path) -> dict[str, object]:
    """Parse enough GIF structure to reject truncation and verify exact timing."""
    data = path.read_bytes()
    if len(data) < 14 or data[:6] not in (b"GIF87a", b"GIF89a"):
        raise MediaError(f"{path} is not a GIF")
    width, height = struct.unpack_from("<HH", data, 6)
    packed = data[10]
    offset = 13 + (3 * (2 ** ((packed & 7) + 1)) if packed & 0x80 else 0)
    frames = 0
    delays: list[int] = []
    pending_delay: int | None = None
    loop: int | None = None

    def subblocks(position: int) -> tuple[list[bytes], int]:
        blocks: list[bytes] = []
        while True:
            if position >= len(data):
                raise MediaError(f"truncated GIF sub-blocks in {path}")
            size = data[position]
            position += 1
            if size == 0:
                return blocks, position
            if position + size > len(data):
                raise MediaError(f"truncated GIF data in {path}")
            blocks.append(data[position:position + size])
            position += size

    while offset < len(data):
        marker = data[offset]
        offset += 1
        if marker == 0x3B:
            if offset != len(data):
                raise MediaError(f"trailing bytes after GIF trailer in {path}")
            break
        if marker == 0x21:
            if offset >= len(data):
                raise MediaError(f"truncated GIF extension in {path}")
            label = data[offset]
            offset += 1
            if label == 0xF9:
                if offset + 6 > len(data) or data[offset] != 4 or data[offset + 5] != 0:
                    raise MediaError(f"malformed graphic-control extension in {path}")
                pending_delay = struct.unpack_from("<H", data, offset + 2)[0] * 10
                offset += 6
            else:
                blocks, offset = subblocks(offset)
                if label == 0xFF and blocks and blocks[0] == b"NETSCAPE2.0":
                    if len(blocks) < 2 or len(blocks[1]) != 3 or blocks[1][0] != 1:
                        raise MediaError(f"malformed GIF loop extension in {path}")
                    loop = struct.unpack_from("<H", blocks[1], 1)[0]
            continue
        if marker == 0x2C:
            if offset + 9 > len(data):
                raise MediaError(f"truncated GIF image descriptor in {path}")
            local_packed = data[offset + 8]
            offset += 9
            if local_packed & 0x80:
                offset += 3 * (2 ** ((local_packed & 7) + 1))
            if offset >= len(data):
                raise MediaError(f"missing GIF image data in {path}")
            offset += 1  # LZW minimum code size
            _, offset = subblocks(offset)
            if pending_delay is None:
                raise MediaError(f"GIF frame lacks an explicit delay in {path}")
            delays.append(pending_delay)
            pending_delay = None
            frames += 1
            continue
        raise MediaError(f"unexpected GIF block 0x{marker:02x} in {path}")
    else:
        raise MediaError(f"GIF trailer is missing in {path}")
    if frames == 0 or loop != 0:
        raise MediaError(f"{path} must be an infinitely looping animated GIF")
    return {"width": width, "height": height, "frames": frames, "delays_ms": delays, "duration_ms": sum(delays)}


def png_info(path: Path) -> dict[str, int]:
    data = path.read_bytes()
    if len(data) < 33 or data[:8] != b"\x89PNG\r\n\x1a\n":
        raise MediaError(f"{path} is not a PNG")
    if struct.unpack_from(">I", data, 8)[0] != 13 or data[12:16] != b"IHDR":
        raise MediaError(f"{path} has no canonical IHDR")
    width, height = struct.unpack_from(">II", data, 16)
    offset = 8
    saw_iend = False
    while offset < len(data):
        if offset + 12 > len(data):
            raise MediaError(f"truncated PNG chunk in {path}")
        length = struct.unpack_from(">I", data, offset)[0]
        end = offset + 12 + length
        if end > len(data):
            raise MediaError(f"truncated PNG data in {path}")
        chunk = data[offset + 4:offset + 8]
        if chunk == b"IEND":
            if length != 0 or end != len(data):
                raise MediaError(f"malformed or non-final PNG IEND in {path}")
            saw_iend = True
        offset = end
    if not saw_iend:
        raise MediaError(f"PNG IEND is missing in {path}")
    return {"width": width, "height": height}


def verify_with_pillow(path: Path, expected_frames: int, expected_size: tuple[int, int]) -> None:
    from PIL import Image

    with Image.open(path) as image:
        image.verify()
    with Image.open(path) as image:
        if image.size != expected_size or getattr(image, "n_frames", 1) != expected_frames:
            raise MediaError(f"Pillow reopen mismatch for {path}")
        for index in range(expected_frames):
            image.seek(index)
            image.convert("RGB").load()


def output_spec(story: dict[str, object], key: str) -> dict[str, object]:
    outputs = story["outputs"]
    assert isinstance(outputs, dict) and isinstance(outputs[key], dict)
    return outputs[key]


def verify_media(
    media: Path,
    story_path: Path,
    mockup_path: Path,
    photo_path: Path | None = None,
    *,
    pillow: bool = False,
) -> dict[str, object]:
    if not media.is_dir() or media.is_symlink():
        raise MediaError(f"media directory is missing or unsafe: {media}")
    story = load_story(story_path)
    _, photo = load_mockup(mockup_path, photo_path)
    manifest_path = media / "manifest.json"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise MediaError(f"cannot read media manifest: {exc}") from exc
    manifest = strict_object(
        manifest,
        {"schema", "story_sha256", "mockup_sha256", "photo_sha256", "frames", "outputs"},
        "media manifest",
    )
    if manifest["schema"] != SCHEMA:
        raise MediaError("media manifest schema mismatch")
    expected_inputs = {
        "story_sha256": sha256(story_path),
        "mockup_sha256": sha256(mockup_path),
        "photo_sha256": sha256(photo),
    }
    for key, expected in expected_inputs.items():
        if manifest[key] != expected:
            raise MediaError(f"media manifest {key} is not bound to the exact tagged input")
    frame_manifest = manifest["frames"]
    if not isinstance(frame_manifest, list):
        raise MediaError("media manifest frames must be an array")
    story_frames = story["frames"]
    assert isinstance(story_frames, list)
    expected_delays = [int(item["delay_ms"]) for item in story_frames]
    expected_names = [str(item["name"]) for item in story_frames]
    if any(
        not isinstance(item, dict) or set(item) != {"name", "label", "delay_ms", "sha256"}
        or not re.fullmatch(r"[0-9a-f]{64}", str(item.get("sha256", "")))
        for item in frame_manifest
    ):
        raise MediaError("media manifest has malformed source-frame evidence")
    actual_names = [item.get("name") for item in frame_manifest]
    actual_labels = [item.get("label") for item in frame_manifest]
    actual_delays = [item.get("delay_ms") for item in frame_manifest]
    expected_labels = [str(item["label"]) for item in story_frames]
    if actual_names != expected_names or actual_labels != expected_labels or actual_delays != expected_delays:
        raise MediaError("media manifest frame order/delays differ from the tagged story")

    outputs = manifest["outputs"]
    if not isinstance(outputs, dict) or set(outputs) != {"hero", "screen", "poster"}:
        raise MediaError("media manifest outputs must be hero, screen and poster")
    for key in ("hero", "screen", "poster"):
        spec = output_spec(story, key)
        filename = str(spec["filename"])
        path = media / filename
        if path.is_symlink() or not path.is_file():
            raise MediaError(f"missing or unsafe media output: {filename}")
        entry = outputs[key]
        if not isinstance(entry, dict) or set(entry) != {"filename", "sha256", "bytes", "width", "height"}:
            raise MediaError(f"invalid manifest entry for {key}")
        expected_entry = {
            "filename": filename,
            "sha256": sha256(path),
            "bytes": path.stat().st_size,
            "width": int(spec["width"]),
            "height": int(spec["height"]),
        }
        if entry != expected_entry:
            raise MediaError(f"media output {filename} does not match its manifest")
        if path.stat().st_size > int(spec["max_bytes"]):
            raise MediaError(f"media output {filename} exceeds its tagged size budget")
        if key in ("hero", "screen"):
            info = gif_info(path)
            expected_info = {
                "width": int(spec["width"]), "height": int(spec["height"]),
                "frames": len(expected_names), "delays_ms": expected_delays,
                "duration_ms": sum(expected_delays),
            }
            if info != expected_info:
                raise MediaError(f"GIF timing/geometry mismatch for {filename}: {info!r}")
            if pillow:
                verify_with_pillow(path, len(expected_names), (int(spec["width"]), int(spec["height"])))
        else:
            if png_info(path) != {"width": int(spec["width"]), "height": int(spec["height"])}:
                raise MediaError(f"PNG geometry mismatch for {filename}")
            if pillow:
                verify_with_pillow(path, 1, (int(spec["width"]), int(spec["height"])))
    actual_files = {path.name for path in media.iterdir() if path.is_file()}
    if actual_files != set(SAFE_MEDIA) or any(path.is_dir() or path.is_symlink() for path in media.iterdir()):
        raise MediaError(f"media directory contains unexpected entries: {sorted(actual_files)!r}")
    return manifest


def build(story_path: Path, frames_dir: Path, mockup_path: Path, output: Path) -> None:
    try:
        from PIL import Image
    except ImportError as exc:
        raise MediaError("Pillow is required to build release media") from exc

    story = load_story(story_path)
    cfg, photo_path = load_mockup(mockup_path)
    if output.exists():
        raise MediaError(f"refusing to replace existing media directory: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix=f".{output.name}-", dir=output.parent))
    try:
        frame_specs = story["frames"]
        assert isinstance(frame_specs, list)
        images = []
        frame_manifest = []
        for item in frame_specs:
            assert isinstance(item, dict)
            name = str(item["name"])
            path = frames_dir / f"{name}.png"
            if path.is_symlink() or not path.is_file():
                raise MediaError(f"required release-HIL frame is missing or unsafe: {name}.png")
            with Image.open(path) as candidate:
                candidate.verify()
            with Image.open(path) as candidate:
                candidate.load()
                if candidate.size != (720, 405) or candidate.mode != "RGB":
                    raise MediaError(f"{name}.png must be a 720x405 RGB release-HIL frame")
                if len(candidate.getcolors(maxcolors=256 * 256) or []) < 4:
                    raise MediaError(f"{name}.png has insufficient rendered content")
                images.append(candidate.copy())
            frame_manifest.append(
                {
                    "name": name, "label": str(item["label"]),
                    "delay_ms": int(item["delay_ms"]), "sha256": sha256(path),
                }
            )

        delays = [int(item["delay_ms"]) for item in frame_specs]
        hero_spec = output_spec(story, "hero")
        screen_spec = output_spec(story, "screen")
        poster_spec = output_spec(story, "poster")
        with Image.open(photo_path) as source_photo:
            source_photo.verify()
        with Image.open(photo_path) as source_photo:
            source_photo.load()
            hero_frames = [composite(source_photo, frame, cfg["corners"], int(hero_spec["width"])) for frame in images]

        hero_palette = hero_frames[0].quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
        hero_rest = [frame.quantize(palette=hero_palette, dither=Image.Dither.NONE) for frame in hero_frames[1:]]
        hero_path = temporary / str(hero_spec["filename"])
        hero_palette.save(
            hero_path, save_all=True, append_images=hero_rest, duration=delays,
            loop=0, optimize=True, disposal=1,
        )

        fixed_palette = Image.new("P", (1, 1))
        fixed_palette.putpalette(rgb332_palette())
        screen_frames = []
        for frame in images:
            exact = frame.resize((240, 135), Image.Resampling.NEAREST)
            exact = exact.resize((int(screen_spec["width"]), int(screen_spec["height"])), Image.Resampling.NEAREST)
            screen_frames.append(exact.quantize(palette=fixed_palette, dither=Image.Dither.NONE))
        screen_path = temporary / str(screen_spec["filename"])
        screen_frames[0].save(
            screen_path, save_all=True, append_images=screen_frames[1:], duration=delays,
            loop=0, optimize=True, disposal=1,
        )

        poster_index = SOURCE_NAMES.index(str(story["poster_frame"]))
        poster_path = temporary / str(poster_spec["filename"])
        hero_frames[poster_index].save(poster_path, format="PNG", optimize=True)

        output_entries: dict[str, object] = {}
        for key, path in (("hero", hero_path), ("screen", screen_path), ("poster", poster_path)):
            spec = output_spec(story, key)
            output_entries[key] = {
                "filename": path.name, "sha256": sha256(path), "bytes": path.stat().st_size,
                "width": int(spec["width"]), "height": int(spec["height"]),
            }
        manifest = {
            "schema": SCHEMA,
            "story_sha256": sha256(story_path),
            "mockup_sha256": sha256(mockup_path),
            "photo_sha256": sha256(photo_path),
            "frames": frame_manifest,
            "outputs": output_entries,
        }
        (temporary / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        verify_media(temporary, story_path, mockup_path, pillow=True)
        temporary.rename(output)
    except Exception:
        shutil.rmtree(temporary, ignore_errors=True)
        raise


def fixture_tokens(path: Path | None) -> list[str]:
    if path is None:
        return []
    try:
        fixture = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise MediaError(f"cannot read redaction fixture: {exc}") from exc
    values: set[str] = set()
    identity_names: set[str] = set()

    def visit(value: object) -> None:
        if isinstance(value, dict):
            for key, child in value.items():
                if isinstance(key, str):
                    values.add(key)
                visit(child)
        elif isinstance(value, list):
            for child in value:
                visit(child)
        elif isinstance(value, str):
            values.add(value)

    visit(fixture)
    devices = fixture.get("devices") if isinstance(fixture, dict) else None
    if isinstance(devices, dict):
        for device in devices.values():
            if isinstance(device, dict) and isinstance(device.get("name"), str):
                identity_names.add(device["name"].strip())
    for collection in ("protected_devices", "wifi_peers"):
        entries = fixture.get(collection) if isinstance(fixture, dict) else None
        if isinstance(entries, list):
            for entry in entries:
                if isinstance(entry, dict) and isinstance(entry.get("name"), str):
                    identity_names.add(entry["name"].strip())
    generic = {
        value for value in values
        if len(value.strip()) >= 4 and value.lower() not in TOKEN_ALLOWLIST
    }
    generic.update(
        name for name in identity_names
        if len(name) >= 1 and name.lower() not in TOKEN_ALLOWLIST
    )
    return sorted(
        generic,
        key=len,
        reverse=True,
    )


def token_pattern(token: str) -> str:
    pattern = re.escape(token)
    return rf"(?<!\w){pattern}(?!\w)" if len(token) < 4 else pattern


def redact_text(value: str, tokens: Iterable[str]) -> str:
    value = USB_RE.sub("<usb-identity>", value)
    value = SERIAL_RE.sub("<serial-port>", value)
    value = IPV4_RE.sub("<local-ip>", value)
    value = IPV6_RE.sub("<local-ipv6>", value)
    value = NODE_RE.sub("<node-id>", value)
    value = NODE_FIELD_RE.sub(r"\1<node-id>", value)
    value = USER_PATH_RE.sub("<user-path>", value)
    value = TEMP_PATH_RE.sub("<temp-path>", value)
    for token in tokens:
        value = re.sub(token_pattern(token), "<lab-redacted>", value, flags=re.IGNORECASE)
    return value


def stage_evidence(source: Path, media: Path | None, fixture: Path | None, output: Path) -> None:
    if not source.is_dir() or source.is_symlink():
        raise MediaError(f"evidence source is missing or unsafe: {source}")
    if output.exists():
        raise MediaError(f"refusing to replace existing public evidence directory: {output}")
    tokens = fixture_tokens(fixture)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix=f".{output.name}-", dir=output.parent))
    copied = 0
    total_bytes = 0
    try:
        for path in sorted(source.rglob("*")):
            if path.is_symlink():
                raise MediaError(f"symlink is forbidden in HIL evidence: {path}")
            if not path.is_file() or path.suffix.lower() not in SAFE_TEXT_SUFFIXES:
                continue
            relative = path.relative_to(source)
            if any(not SAFE_PART_RE.fullmatch(part) for part in relative.parts):
                raise MediaError(f"unsafe public evidence path: {relative}")
            if any(re.search(token_pattern(token), str(relative), re.IGNORECASE) for token in tokens):
                raise MediaError(f"fixture token appears in public evidence path: {relative}")
            size = path.stat().st_size
            if size > MAX_TEXT_FILE_BYTES:
                raise MediaError(f"public evidence file exceeds byte budget: {relative}")
            total_bytes += size
            if total_bytes > MAX_TEXT_TOTAL_BYTES:
                raise MediaError("public text evidence exceeds total byte budget")
            try:
                raw = path.read_text(encoding="utf-8")
            except (OSError, UnicodeError) as exc:
                raise MediaError(f"public evidence is not UTF-8 text: {relative}") from exc
            clean = redact_text(raw, tokens)
            for token in tokens:
                if re.search(token_pattern(token), clean, re.IGNORECASE):
                    raise MediaError(f"fixture token survived evidence redaction: {relative}")
            if path.suffix.lower() == ".json":
                json.loads(clean)
            elif path.suffix.lower() == ".xml":
                ET.fromstring(clean)
            destination = temporary / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(clean, encoding="utf-8")
            copied += 1
        if copied == 0:
            raise MediaError("no safe textual HIL evidence was found")
        if media is not None:
            if not media.is_dir() or media.is_symlink():
                raise MediaError(f"safe media directory is missing or unsafe: {media}")
            media_out = temporary / "media"
            media_out.mkdir()
            for name in SAFE_MEDIA:
                source_path = media / name
                if source_path.is_symlink() or not source_path.is_file():
                    raise MediaError(f"safe media is incomplete: {name}")
                shutil.copyfile(source_path, media_out / name)
        forbidden = [path for path in temporary.rglob("*") if path.suffix.lower() in {".png", ".rgb332"}]
        allowed_poster = temporary / "media" / "hero.png"
        if any(path != allowed_poster for path in forbidden):
            raise MediaError("raw visual capture entered public evidence")
        temporary.rename(output)
    except Exception:
        shutil.rmtree(temporary, ignore_errors=True)
        raise


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)

    build_command = commands.add_parser("build", help="build exact media from release-HIL PNG frames")
    build_command.add_argument("--story", type=Path, required=True)
    build_command.add_argument("--frames", type=Path, required=True)
    build_command.add_argument("--mockup", type=Path, required=True)
    build_command.add_argument("--output", type=Path, required=True)

    verify_command = commands.add_parser("verify", help="verify downloaded media using exact tagged inputs")
    verify_command.add_argument("--story", type=Path, required=True)
    verify_command.add_argument("--mockup", type=Path, required=True)
    verify_command.add_argument("--photo", type=Path)
    verify_command.add_argument("--media", type=Path, required=True)

    evidence_command = commands.add_parser("stage-evidence", help="stage redacted public HIL evidence")
    evidence_command.add_argument("--source", type=Path, required=True)
    evidence_command.add_argument("--media", type=Path)
    evidence_command.add_argument("--fixture", type=Path)
    evidence_command.add_argument("--output", type=Path, required=True)

    args = parser.parse_args()
    try:
        if args.command == "build":
            build(args.story, args.frames, args.mockup, args.output)
            print(f"media: {args.output}")
        elif args.command == "verify":
            manifest = verify_media(args.media, args.story, args.mockup, args.photo)
            outputs = manifest["outputs"]
            assert isinstance(outputs, dict)
            print("media verified:", ", ".join(str(outputs[key]["sha256"]) for key in ("hero", "screen", "poster")))
        else:
            stage_evidence(args.source, args.media, args.fixture, args.output)
            print(f"public evidence: {args.output}")
    except (MediaError, OSError) as exc:
        print(f"demo media error: {exc}", file=sys.stderr)
        raise SystemExit(2) from exc


if __name__ == "__main__":
    main()
