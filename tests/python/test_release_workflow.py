import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = (ROOT / ".github/workflows/build.yml").read_text()


def job(name: str) -> str:
    """Extract one top-level Actions job without adding a YAML dependency."""
    match = re.search(rf"(?ms)^  {re.escape(name)}:\n(.*?)(?=^  [a-zA-Z0-9_-]+:\n|\Z)", WORKFLOW)
    if not match:
        raise AssertionError(f"workflow job {name!r} is missing")
    return match.group(0)


class ReleaseWorkflowTests(unittest.TestCase):
    def test_manual_flash_stops_on_overlay_sync_failure(self):
        script = (ROOT / "scripts/flash.sh").read_text()
        self.assertIn("set -euo pipefail", script)
        self.assertIn('"$ROOT/scripts/sync-overlay.sh"', script)

    def test_physical_hil_is_tag_only_and_identity_bound(self):
        block = job("release-hil")
        self.assertIn("if: startsWith(github.ref, 'refs/tags/v')", block)
        self.assertIn("needs: [host-tests, firmware]", block)
        self.assertIn("runs-on: [self-hosted, macOS, meshtastic-adv-hil]", block)
        self.assertIn("environment: release-hil", block)
        self.assertIn("secrets.HIL_FIXTURE_JSON", block)
        self.assertIn("umask 077", block)
        self.assertIn("--release-image", block)
        self.assertNotIn("runner.temp", block.split("steps:", 1)[0])
        self.assertEqual(block.count("${{ runner.temp }}"), 3)

    def test_physical_hil_uses_relocatable_runner_python(self):
        block = job("release-hil")
        self.assertNotIn("actions/setup-python", block)
        self.assertIn("command -v python3.12", block)
        self.assertIn('python3.12 -m venv "$RUNNER_TEMP/meshtastic-adv-hil-venv"', block)
        self.assertIn('echo "$RUNNER_TEMP/meshtastic-adv-hil-venv/bin" >> "$GITHUB_PATH"', block)
        self.assertIn(
            "python -m pip install --require-hashes --no-deps -r requirements/pip.txt",
            block,
        )
        self.assertIn("python -m pip install --require-hashes -r hil/requirements.txt", block)
        self.assertIn(
            "python -m pip install --require-hashes --no-deps -r requirements/platformio.txt",
            block,
        )

    def test_host_ci_runs_actionlint(self):
        block = job("host-tests")
        self.assertIn("docker://rhysd/actionlint@sha256:", block)

    def test_malformed_release_tag_cannot_reach_physical_hil(self):
        host = job("host-tests")
        self.assertIn("if: startsWith(github.ref, 'refs/tags/v')", host)
        self.assertIn('python scripts/release_tag.py "$GITHUB_REF_NAME"', host)
        physical = job("release-hil")
        self.assertIn("needs: [host-tests, firmware]", physical)

    def test_release_cannot_bypass_behavioral_hil(self):
        block = job("release")
        self.assertIn("if: startsWith(github.ref, 'refs/tags/v')", block)
        self.assertIn("needs: [host-tests, firmware, release-hil]", block)
        self.assertIn("group: meshtastic-adv-release-publish", block)
        self.assertIn("cancel-in-progress: false", block)
        self.assertNotIn("continue-on-error: true", block.split("- name: Create the GitHub release", 1)[0])

    def test_release_app_is_proven_to_be_inside_factory_image(self):
        firmware = job("firmware")
        physical = job("release-hil")
        self.assertRegex(firmware, r"(?s)name: firmware-factory.*steps\.img\.outputs\.factory.*steps\.img\.outputs\.app")
        self.assertIn('f[0x10000:0x10000+len(a)] != a', physical)
        self.assertNotIn('assert f[0x10000:', physical)
        self.assertIn('echo "path=$app" >> "$GITHUB_OUTPUT"', physical)

    def test_secret_fixture_cleanup_runs_even_after_failure(self):
        block = job("release-hil")
        cleanup = block.split("- name: Remove the identity-bound temporary fixture", 1)[1]
        self.assertIn("if: always()", cleanup)
        self.assertIn('rm -f -- "$HIL_FIXTURE"', cleanup)

    def test_release_uses_the_verified_merged_image_builder(self):
        block = job("release")
        self.assertIn("bash /tmp/mkm5burner.sh", block)
        self.assertIn("MESHTASTIC_FACTORY=/tmp/fw.bin", block)
        self.assertNotIn("python -m esptool --chip esp32s3 merge_bin", block)

    def test_release_assets_remain_bound_to_tag_after_main_checkout(self):
        block = job("release")
        staging = block.index("- name: Stage exact tagged release inputs")
        checkout_main = block.index("git checkout -B main origin/main")
        publish = block.index("- name: Create the GitHub release")
        self.assertLess(staging, checkout_main)
        self.assertLess(checkout_main, publish)
        self.assertIn("cp docs/unifont.bin /tmp/unifont.bin", block[:checkout_main])
        self.assertIn("cp scripts/check-installer.py /tmp/check-installer.py", block[:checkout_main])
        self.assertIn("cp scripts/mkm5burner.sh /tmp/mkm5burner.sh", block[:checkout_main])
        self.assertIn("cp scripts/verify_web_publish.py /tmp/verify_web_publish.py", block[:checkout_main])
        self.assertIn("cp requirements/release.txt /tmp/release-requirements.txt", block[:checkout_main])
        self.assertIn(
            'python scripts/release_notes.py "$GITHUB_REF_NAME" --output /tmp/release-notes.md',
            block[:checkout_main],
        )
        self.assertIn("pip install --require-hashes -r /tmp/release-requirements.txt", block[:checkout_main])
        self.assertIn('echo "engine=$engine" >> "$GITHUB_OUTPUT"', block[:checkout_main])
        self.assertIn('python scripts/release_tag.py "$GITHUB_REF_NAME" >> "$GITHUB_OUTPUT"', block[:checkout_main])
        self.assertIn("cp /tmp/unifont.bin docs/unifont.bin", block[checkout_main:publish])
        self.assertIn('engine="${{ steps.tagged.outputs.engine }}"', block[staging:checkout_main])
        self.assertIn("python /tmp/check-installer.py --prune docs", block[checkout_main:publish])
        self.assertNotIn("xargs -r rm -rf", block)
        self.assertIn("python /tmp/verify_web_publish.py", block[checkout_main:publish])
        self.assertIn("--firmware /tmp/fw.bin", block[checkout_main:publish])
        self.assertIn("--font /tmp/unifont.bin", block[checkout_main:publish])
        self.assertIn("MESHTASTIC_FONT=/tmp/unifont.bin", block[publish:])
        self.assertIn("python /tmp/m5burner_publish.py", block[publish:])

    def test_stable_release_uses_exact_tag_changelog_notes_and_reruns_repair_them(self):
        block = job("release")
        staging = block.index("- name: Stage exact tagged release inputs")
        checkout_main = block.index("git checkout -B main origin/main")
        publish = block.index("- name: Create the GitHub release")
        self.assertLess(staging, checkout_main)
        self.assertLess(checkout_main, publish)
        self.assertIn("notes_flags=(--generate-notes)", block[publish:])
        self.assertIn("notes_flags=(--notes-file /tmp/release-notes.md)", block[publish:])
        self.assertIn('gh release edit "${GITHUB_REF_NAME}" --notes-file /tmp/release-notes.md', block[publish:])
        self.assertNotIn("--generate-notes \\", block[publish:])

    def test_prerelease_cannot_replace_stable_distribution_channels(self):
        block = job("release")
        installer = block.split("- name: Refresh the installer image on main", 1)[1]
        installer = installer.split("- name: Create the GitHub release", 1)[0]
        release = block.split("- name: Create the GitHub release", 1)[1]
        m5burner = release.split("- name: Publish to M5Burner", 1)[1]
        self.assertIn("if: steps.tagged.outputs.prerelease == 'false'", installer)
        self.assertIn("release_flags+=(--prerelease --latest=false)", release)
        self.assertIn('"${release_flags[@]}"', release)
        self.assertIn("--verify-tag", release)
        self.assertIn("if: steps.tagged.outputs.prerelease == 'false'", m5burner)
        local_publisher = (ROOT / "scripts/publish-firmware.sh").read_text()
        self.assertIn("invalid stable release version", local_publisher)
        self.assertNotIn("(?:[-+][0-9A-Za-z.-]+)?", local_publisher)

    def test_m5burner_publisher_never_sends_auth_over_plain_http(self):
        publisher = (ROOT / "scripts/m5burner_publish.py").read_text()
        self.assertIn('API = "https://m5burner-api.m5stack.com"', publisher)
        self.assertNotIn('API = "http://', publisher)

    def test_stable_release_fails_closed_until_m5burner_is_public(self):
        block = job("release")
        m5burner = block.split("- name: Publish to M5Burner", 1)[1]
        self.assertNotIn("continue-on-error", m5burner)
        self.assertIn('echo "M5Burner secrets not set"', m5burner)
        self.assertIn("exit 1", m5burner)
        self.assertNotIn("pip install", m5burner)
        publisher = (ROOT / "scripts/m5burner_publish.py").read_text()
        self.assertIn("verify_public_version", publisher)
        self.assertIn('public.get("published") is True', publisher)

    def test_external_actions_and_python_installs_are_immutable(self):
        for reference in re.findall(r"(?m)^\s+uses:\s+([^\s#]+)", WORKFLOW):
            if reference.startswith("docker://"):
                self.assertIn("@sha256:", reference)
            else:
                self.assertRegex(reference, r"@[0-9a-f]{40}$")
        self.assertNotRegex(WORKFLOW, r"pip install ['\"]?[^\n]*[<>=]")
        self.assertEqual(WORKFLOW.count("pip install --require-hashes -r"), 3)
        self.assertEqual(WORKFLOW.count("pip install --require-hashes --no-deps"), 5)

    def test_release_tooling_excludes_fixed_python_vulnerabilities(self):
        for relative in ("hil/requirements.txt", "requirements/release.txt"):
            lock = (ROOT / relative).read_text()
            self.assertRegex(lock, r"(?m)^esptool==5\.")
            self.assertNotRegex(lock, r"(?m)^ecdsa==")
            self.assertIn(
                "cryptography==50.0.1 ; platform_machine != 'x86_64' or sys_platform != 'darwin'",
                lock,
            )
        hil_lock = (ROOT / "hil/requirements.txt").read_text()
        self.assertNotRegex(hil_lock, r"(?m)^(?:starlette|uvicorn|wsproto|ajsonrpc)==")
        self.assertRegex(hil_lock, r"(?m)^grpcio-tools==")
        platformio_lock = (ROOT / "requirements/platformio.txt").read_text()
        self.assertRegex(platformio_lock, r"(?m)^platformio==6\.1\.19 \\")
        pip_lock = (ROOT / "requirements/pip.txt").read_text()
        self.assertRegex(pip_lock, r"(?m)^pip==26\.2\.1 \\")

    def test_installer_commit_errors_are_not_treated_as_no_change(self):
        block = job("release")
        self.assertIn("if git diff --cached --quiet; then", block)
        self.assertNotIn('git commit -m "ci: web installer image for ${GITHUB_REF_NAME} [skip ci]" ||', block)

    def test_documented_release_asset_names_match_the_workflow(self):
        block = job("release")
        self.assertIn('"meshtastic-adv-${GITHUB_REF_NAME}.factory.bin"', block)
        for readme in ("README.md", "README.ru.md"):
            text = (ROOT / readme).read_text()
            self.assertIn("meshtastic-adv-vX.Y.Z.factory.bin", text)
            self.assertIn("meshtastic-adv-cardputer-adv-merged.bin", text)

    def test_merged_image_builder_is_esptool5_safe_and_fail_closed(self):
        script = (ROOT / "scripts/mkm5burner.sh").read_text()
        self.assertIn("merge-bin", script)
        self.assertIn("--flash-mode", script)
        self.assertIn("--flash-size", script)
        self.assertNotIn("merge_bin", script)
        self.assertNotIn("--flash_mode", script)
        self.assertNotIn("|| true", script)
        self.assertNotIn("assert ", script)
        self.assertIn('rm -f -- "$OUT"', script)
        self.assertIn('require(b[:len(factory)] == factory', script)
        self.assertIn('require(b[off:off+len(font)] == font', script)

    def test_merged_image_builder_cannot_validate_a_stale_output_after_merge_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            factory = temp / "factory.bin"
            font = temp / "font.bin"
            output = temp / "merged.bin"
            fake_esptool = temp / "esptool"
            staged_builder = temp / "mkm5burner.sh"
            factory.write_bytes(b"\xe9" + bytes(31))
            font.write_bytes(b"AUF2" + bytes(31))
            output.write_bytes(factory.read_bytes() + bytes(0x340000 - factory.stat().st_size) + font.read_bytes())
            fake_esptool.write_text("#!/bin/sh\nexit 7\n")
            fake_esptool.chmod(0o700)
            staged_builder.write_bytes((ROOT / "scripts/mkm5burner.sh").read_bytes())
            env = {
                **os.environ,
                "ESPTOOL": str(fake_esptool),
                "MESHTASTIC_FACTORY": str(factory),
                "MESHTASTIC_FONT": str(font),
                "MESHTASTIC_MERGED_OUT": str(output),
                "MESHTASTIC_REPO_ROOT": str(ROOT),
            }
            result = subprocess.run(
                ["bash", str(staged_builder), "1.2.3"],
                cwd=ROOT,
                env=env,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(result.returncode, 7)
            self.assertFalse(output.exists())

    def test_merged_image_builder_rejects_and_removes_invalid_fresh_output_under_python_optimized(self):
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            factory = temp / "factory.bin"
            font = temp / "font.bin"
            output = temp / "merged.bin"
            fake_esptool = temp / "esptool"
            staged_builder = temp / "mkm5burner.sh"
            factory.write_bytes(b"\xe9" + bytes(31))
            font.write_bytes(b"AUF2" + bytes(31))
            fake_esptool.write_text(
                "#!/bin/sh\n"
                "out=\n"
                "while [ \"$#\" -gt 0 ]; do\n"
                "  if [ \"$1\" = -o ]; then shift; out=$1; fi\n"
                "  shift\n"
                "done\n"
                "printf '\\351' > \"$out\"\n"
            )
            fake_esptool.chmod(0o700)
            staged_builder.write_bytes((ROOT / "scripts/mkm5burner.sh").read_bytes())
            env = {
                **os.environ,
                "PYTHONOPTIMIZE": "1",
                "ESPTOOL": str(fake_esptool),
                "MESHTASTIC_FACTORY": str(factory),
                "MESHTASTIC_FONT": str(font),
                "MESHTASTIC_MERGED_OUT": str(output),
                "MESHTASTIC_REPO_ROOT": str(ROOT),
            }
            result = subprocess.run(
                ["bash", str(staged_builder), "1.2.3"],
                cwd=ROOT,
                env=env,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
