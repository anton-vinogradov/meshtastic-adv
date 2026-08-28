import hashlib
import importlib.util
import json
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

try:
    from PIL import Image, ImageDraw
except ImportError:
    Image = ImageDraw = None


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("demo_media", ROOT / "scripts/demo_media.py")
demo_media = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = demo_media
SPEC.loader.exec_module(demo_media)


def animated_gif(width, height, delays):
    data = bytearray(b"GIF89a")
    data.extend(struct.pack("<HHBBB", width, height, 0x80, 0, 0))
    data.extend(b"\x00\x00\x00\xff\xff\xff")
    data.extend(b"\x21\xff\x0bNETSCAPE2.0\x03\x01\x00\x00\x00")
    for delay in delays:
        data.extend(b"\x21\xf9\x04\x00")
        data.extend(struct.pack("<H", delay // 10))
        data.extend(b"\x00\x00")
        data.extend(b"\x2c\x00\x00\x00\x00\x01\x00\x01\x00\x00")
        data.extend(b"\x02\x02\x44\x01\x00")
    data.append(0x3B)
    return bytes(data)


def png(width, height):
    def chunk(name, value):
        return struct.pack(">I", len(value)) + name + value + struct.pack(">I", zlib.crc32(name + value) & 0xFFFFFFFF)

    raw = b"\x00" + b"\x00\x00\x00" * width
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw * height))
        + chunk(b"IEND", b"")
    )


class DemoMediaTests(unittest.TestCase):
    def test_tracked_public_media_is_bound_to_the_reviewed_story(self):
        with tempfile.TemporaryDirectory() as directory:
            media = Path(directory)
            for name in ("hero.gif", "demo.gif", "hero.png"):
                (media / name).write_bytes((ROOT / "docs/img" / name).read_bytes())
            (media / "manifest.json").write_bytes(
                (ROOT / "docs/img/demo-manifest.json").read_bytes()
            )
            demo_media.verify_media(
                media,
                ROOT / "docs/img/demo-story.json",
                ROOT / "docs/img/mockup.json",
            )

    def test_story_fixes_the_complete_frame_order_delays_and_budgets(self):
        story = demo_media.load_story(ROOT / "docs/img/demo-story.json")
        frames = story["frames"]
        self.assertEqual([item["name"] for item in frames], [f"a{i:02d}" for i in range(1, 20)])
        self.assertEqual(sum(item["delay_ms"] for item in frames), 16_160)
        self.assertLessEqual(sum(item["delay_ms"] for item in frames), story["max_duration_ms"])
        self.assertEqual(story["outputs"]["hero"]["filename"], "hero.gif")
        self.assertEqual(story["outputs"]["screen"]["filename"], "demo.gif")

    def test_story_rejects_reordered_or_oversized_input(self):
        original = json.loads((ROOT / "docs/img/demo-story.json").read_text())
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "story.json"
            original["frames"][0], original["frames"][1] = original["frames"][1], original["frames"][0]
            path.write_text(json.dumps(original))
            with self.assertRaisesRegex(demo_media.MediaError, "set/order"):
                demo_media.load_story(path)
            original = json.loads((ROOT / "docs/img/demo-story.json").read_text())
            original["outputs"]["hero"]["max_bytes"] = 4_000_001
            path.write_text(json.dumps(original))
            with self.assertRaisesRegex(demo_media.MediaError, "4 MB"):
                demo_media.load_story(path)

    def test_gif_parser_requires_exact_loop_trailer_and_delays(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "demo.gif"
            path.write_bytes(animated_gif(960, 540, [100, 240]))
            self.assertEqual(
                demo_media.gif_info(path),
                {"width": 960, "height": 540, "frames": 2, "delays_ms": [100, 240], "duration_ms": 340},
            )
            path.write_bytes(path.read_bytes()[:-1])
            with self.assertRaisesRegex(demo_media.MediaError, "trailer"):
                demo_media.gif_info(path)

    def test_downloaded_media_is_bound_to_tagged_story_mockup_and_photo(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            media = root / "media"
            media.mkdir()
            story = ROOT / "docs/img/demo-story.json"
            mockup = root / "mockup.json"
            photo = root / "device.png"
            mockup.write_text(json.dumps({"photo": "device.png", "corners": [[1, 1], [2, 1], [2, 2], [1, 2]]}))
            photo.write_bytes(b"exact tagged photo")
            story_value = json.loads(story.read_text())
            delays = [item["delay_ms"] for item in story_value["frames"]]
            (media / "hero.gif").write_bytes(animated_gif(900, 600, delays))
            (media / "demo.gif").write_bytes(animated_gif(960, 540, delays))
            (media / "hero.png").write_bytes(png(900, 600))
            outputs = {}
            for key, filename, width, height in (
                ("hero", "hero.gif", 900, 600),
                ("screen", "demo.gif", 960, 540),
                ("poster", "hero.png", 900, 600),
            ):
                path = media / filename
                outputs[key] = {
                    "filename": filename,
                    "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                    "bytes": path.stat().st_size,
                    "width": width,
                    "height": height,
                }
            manifest = {
                "schema": 1,
                "story_sha256": demo_media.sha256(story),
                "mockup_sha256": demo_media.sha256(mockup),
                "photo_sha256": demo_media.sha256(photo),
                "frames": [
                    {
                        "name": item["name"], "label": item["label"],
                        "delay_ms": item["delay_ms"], "sha256": "a" * 64,
                    }
                    for item in story_value["frames"]
                ],
                "outputs": outputs,
            }
            (media / "manifest.json").write_text(json.dumps(manifest))
            demo_media.verify_media(media, story, mockup, photo)
            photo.write_bytes(b"different photo")
            with self.assertRaisesRegex(demo_media.MediaError, "exact tagged input"):
                demo_media.verify_media(media, story, mockup, photo)

    def test_public_evidence_excludes_raw_frames_and_redacts_fixture_names(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "private"
            source.mkdir()
            (source / "summary.json").write_text(
                json.dumps({"dut": "Private DUT", "protected": "Protected Node", "node": "!abcdef12"})
            )
            logs = source / "logs"
            logs.mkdir()
            (logs / "hil.log").write_text(
                "Protected Node N1 S2 LAB7 at 192.168.99.77 via /dev/cu.usbmodem-test "
                "AA:BB:CC:DD:EE:FF; Z9 standalone; embeddedN1name stays; prefixZ9suffix stays "
                "/private/var/folders/39/stable-user-token/T/hil/release.bin\n"
            )
            frames = source / "visual" / "frames"
            frames.mkdir(parents=True)
            (frames / "a01.png").write_bytes(b"private png")
            (frames / "a01.rgb332").write_bytes(b"private framebuffer")
            fixture = root / "fixture.json"
            fixture.write_text(json.dumps({
                "devices": {"dut": {"name": "Private DUT"}, "Protected Node": {}},
                "protected_devices": [{"name": "LAB7"}, {"name": "Z9"}],
                "wifi_peers": [{"name": "N1"}, {"name": "S2"}],
            }))
            public = root / "public"
            demo_media.stage_evidence(source, None, fixture, public)
            published = "\n".join(path.read_text() for path in public.rglob("*") if path.is_file())
            self.assertNotIn("Protected Node", published)
            self.assertNotIn("Private DUT", published)
            self.assertNotIn(" N1 ", published)
            self.assertNotIn(" S2 ", published)
            self.assertNotIn(" LAB7 ", published)
            self.assertNotIn(" Z9 standalone", published)
            self.assertIn("embeddedN1name stays", published)
            self.assertIn("prefixZ9suffix stays", published)
            self.assertNotIn("192.168.99.77", published)
            self.assertNotIn("usbmodem-test", published)
            self.assertNotIn("stable-user-token", published)
            self.assertIn("<temp-path>", published)
            self.assertFalse(any(path.suffix in {".png", ".rgb332"} for path in public.rglob("*")))

    def test_public_evidence_fails_on_symlink(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "private"
            source.mkdir()
            (source / "report.json").write_text("{}")
            (source / "escape.log").symlink_to(source / "report.json")
            with self.assertRaisesRegex(demo_media.MediaError, "symlink"):
                demo_media.stage_evidence(source, None, None, root / "public")

    def test_redaction_removes_complete_local_ipv4_addresses(self):
        clean = demo_media.redact_text(
            "10.1.2.3 127.0.0.1 169.254.4.5 172.31.9.8 192.168.99.77",
            [],
        )
        self.assertEqual(clean, "<local-ip> <local-ip> <local-ip> <local-ip> <local-ip>")
        self.assertNotRegex(clean, r"(?:^|\s)\.[0-9]{1,3}(?:\s|$)")

    def test_redaction_removes_user_scoped_and_generic_unix_temp_paths(self):
        clean = demo_media.redact_text(
            "/var/folders/39/stable-user-token/T/tmp123/fixture.json "
            "/private/var/folders/ab/another-token/C/cache.bin "
            "/tmp/job/output.log /private/tmp/venv/bin/python "
            "/Users/alice/Documents/private-project/file.bin "
            "/home/runner/work/project/output.log",
            [],
        )
        self.assertEqual(
            clean,
            "<temp-path> <temp-path> <temp-path> <temp-path> <user-path> <user-path>",
        )

    def test_redaction_removes_bare_node_fields_and_local_ipv6(self):
        clean = demo_media.redact_text(
            'node=abcdef12 my: "1234ABCD" selected=feedbeef last=87654321 '
            'id=01020304 from=11223344 to=55667788 reply=90abcdef '
            'tx_reply=13572468 react_last_target=24681357 fe80::1%en0 fd12:3456::9 ::1',
            [],
        )
        self.assertEqual(
            clean,
            'node=<node-id> my: "<node-id>" selected=<node-id> last=<node-id> '
            'id=<node-id> from=<node-id> to=<node-id> reply=<node-id> '
            'tx_reply=<node-id> react_last_target=<node-id> '
            '<local-ipv6> <local-ipv6> <local-ipv6>',
        )

    def test_redaction_removes_node_fields_from_json_reports(self):
        clean = demo_media.redact_text(
            '{"node": "abcdef12", "selected": "1234abcd", "last": "feedbeef"}',
            [],
        )
        self.assertEqual(
            clean,
            '{"node": "<node-id>", "selected": "<node-id>", "last": "<node-id>"}',
        )

    def test_public_evidence_enforces_per_file_byte_budget(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "private"
            source.mkdir()
            (source / "too-large.log").write_bytes(b"x" * (demo_media.MAX_TEXT_FILE_BYTES + 1))
            with self.assertRaisesRegex(demo_media.MediaError, "byte budget"):
                demo_media.stage_evidence(source, None, None, root / "public")

    @unittest.skipUnless(Image is not None, "Pillow is installed in the firmware CI job")
    def test_pillow_end_to_end_build_reopens_exact_outputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            frames = root / "frames"
            frames.mkdir()
            for index, name in enumerate(demo_media.SOURCE_NAMES):
                frame = Image.new("RGB", (720, 405), (5, 6, 10))
                draw = ImageDraw.Draw(frame)
                draw.rectangle((20, 20, 700, 385), outline=(55, 225, 255), width=5)
                draw.rectangle((40, 70 + index, 500, 130 + index), fill=(40, 60, 90))
                draw.text((50, 40), name, fill=(255, 207, 63))
                frame.save(frames / f"{name}.png")
            output = root / "media"
            demo_media.build(
                ROOT / "docs/img/demo-story.json",
                frames,
                ROOT / "docs/img/mockup.json",
                output,
            )
            manifest = demo_media.verify_media(
                output,
                ROOT / "docs/img/demo-story.json",
                ROOT / "docs/img/mockup.json",
                pillow=True,
            )
            self.assertEqual(len(manifest["frames"]), 19)
            cover_spec = importlib.util.spec_from_file_location("mkcover", ROOT / "scripts/mkcover.py")
            mkcover = importlib.util.module_from_spec(cover_spec)
            cover_spec.loader.exec_module(mkcover)
            cover = root / "m5burner-cover.png"
            mkcover.build(output / "demo.gif", cover)
            with Image.open(cover) as image:
                self.assertEqual(image.size, (1000, 1000))


if __name__ == "__main__":
    unittest.main()
