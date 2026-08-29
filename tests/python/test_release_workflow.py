import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = (ROOT / ".github/workflows/build.yml").read_text()
REPAIR_WORKFLOW = (ROOT / ".github/workflows/repair-m5burner.yml").read_text()
FINISH_WORKFLOW = (ROOT / ".github/workflows/finish-release.yml").read_text()


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
        self.assertIn("fetch-depth: 0", block)
        self.assertIn(
            "git fetch --no-tags origin main:refs/remotes/origin/main", block
        )
        self.assertIn("git merge-base --is-ancestor HEAD origin/main", block)
        self.assertIn("secrets.HIL_FIXTURE_JSON", block)
        self.assertGreaterEqual(block.count("github.run_attempt"), 7)
        self.assertIn("os.O_EXCL", block)
        self.assertIn('getattr(os,"O_NOFOLLOW",0)', block)
        self.assertIn("os.fsync", block)
        self.assertIn("umask 077", block)
        self.assertIn("--release-image", block)
        self.assertIn("--production-wifi", block)
        self.assertIn("--hardware-only", block)
        self.assertIn("--skip-build", block)
        self.assertIn("name: firmware-hil-private", block)
        self.assertIn("path: firmware/.pio/build/m5stack-cardputer-adv-advui-hil", block)
        self.assertNotIn("runner.temp", block.split("steps:", 1)[0])
        self.assertEqual(block.count("${{ runner.temp }}"), 5)
        self.assertIn("timeout-minutes: 150", block)
        self.assertIn("queue: max", block)

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
        self.assertNotIn("requirements/platformio.txt", block)

    def test_physical_hil_recovery_vault_survives_checkout_and_is_exactly_cleaned(self):
        block = job("release-hil")
        header = block.split("steps:", 1)[0]
        self.assertIn("HIL_RECOVERY_ROOT: ${{ vars.HIL_RECOVERY_ROOT }}", header)
        self.assertIn("run-${{ github.run_id }}-attempt-${{ github.run_attempt }}", header)
        self.assertIn("/config-before.yaml", header)
        self.assertIn("/hil-flash-started", header)
        prepare = block.split("- name: Prepare persistent configuration recovery vault", 1)[1]
        prepare = prepare.split("- uses: actions/download-artifact", 1)[0]
        self.assertIn('test ! -e "$recovery_dir"', prepare)
        self.assertIn('mkdir -m 700 -- "$recovery_dir"', prepare)
        self.assertIn("outside checkout/temp", prepare)
        run = block.split("- name: Release behavioral HIL", 1)[1]
        run = run.split("- name: Recover production firmware", 1)[0]
        self.assertIn('--config-backup "$HIL_CONFIG_BACKUP"', run)
        recovery = block.split("- name: Recover production firmware", 1)[1]
        recovery = recovery.split("- name: Build deterministic release-demo media", 1)[0]
        self.assertIn('--config-backup "$HIL_CONFIG_BACKUP"', recovery)
        self.assertIn('--flash-marker "$HIL_FLASH_MARKER"', recovery)
        self.assertIn('if [ ! -f "$HIL_FLASH_MARKER" ]', recovery)
        self.assertIn("recovery is not required", recovery)
        cleanup = block.split(
            "- name: Remove private configuration backup after verified recovery", 1
        )[1].split("- name: Remove the identity-bound temporary fixture", 1)[0]
        self.assertIn("steps.recovery.outcome == 'success'", cleanup)
        self.assertIn("run-${GITHUB_RUN_ID}-attempt-${GITHUB_RUN_ATTEMPT}", cleanup)
        self.assertIn('test "$HIL_CONFIG_BACKUP" = "$expected"', cleanup)
        self.assertIn('test "$HIL_FLASH_MARKER" = "$marker"', cleanup)
        self.assertIn('rm -f -- "$HIL_CONFIG_BACKUP" "$HIL_FLASH_MARKER"', cleanup)
        self.assertIn('rmdir -- "$(dirname "$HIL_CONFIG_BACKUP")"', cleanup)
        self.assertNotIn("hil-artifacts/release-hil/usb/private/config-before.yaml", block)

    def test_host_ci_runs_actionlint(self):
        block = job("host-tests")
        self.assertIn("docker run --rm", block)
        self.assertIn('--volume "$GITHUB_WORKSPACE:/repo:ro" --workdir /repo', block)
        self.assertIn(
            "rhysd/actionlint@sha256:b1934ee5f1c509618f2508e6eb47ee0d3520686341fec936f3b79331f9315667",
            block,
        )
        self.assertIn(
            "-ignore 'unexpected key \"queue\" for \"concurrency\" section'",
            block,
        )

    def test_malformed_release_tag_cannot_reach_physical_hil(self):
        host = job("host-tests")
        self.assertIn("if: startsWith(github.ref, 'refs/tags/v')", host)
        self.assertIn('python scripts/release_tag.py "$GITHUB_REF_NAME"', host)
        self.assertIn('python scripts/release_notes.py "$GITHUB_REF_NAME" >/dev/null', host)
        physical = job("release-hil")
        self.assertIn("needs: [host-tests, firmware]", physical)

    def test_release_cannot_bypass_behavioral_hil(self):
        block = job("release")
        self.assertIn("if: startsWith(github.ref, 'refs/tags/v')", block)
        self.assertIn("needs: [host-tests, firmware, release-hil]", block)
        self.assertIn("group: meshtastic-adv-release-publish", block)
        self.assertIn("queue: max", block)
        self.assertIn("cancel-in-progress: false", block)
        self.assertNotIn("continue-on-error: true", block)
        self.assertIn("timeout-minutes: 90", block)

    def test_release_app_is_proven_to_be_inside_factory_image(self):
        firmware = job("firmware")
        physical = job("release-hil")
        self.assertRegex(firmware, r"(?s)name: firmware-factory.*steps\.img\.outputs\.factory.*steps\.img\.outputs\.app")
        self.assertIn('f[0x10000:0x10000+len(a)] != a', physical)
        self.assertNotIn('assert f[0x10000:', physical)
        self.assertIn('echo "path=$app" >> "$GITHUB_OUTPUT"', physical)
        self.assertIn("name: firmware-hil-private", firmware)
        self.assertIn("steps.img.outputs.hil_app", firmware)

    def test_secret_fixture_cleanup_runs_even_after_failure(self):
        block = job("release-hil")
        recovery = block.split("- name: Recover production firmware after HIL interruption", 1)[1]
        recovery = recovery.split("- name: Build deterministic release-demo media", 1)[0]
        self.assertIn("if: always() && steps.hil.outcome != 'success'", recovery)
        self.assertIn('python scripts/hil.py restore', recovery)
        self.assertIn('--image "${{ steps.release_app.outputs.path }}"', recovery)
        private_cleanup = block.split(
            "- name: Remove private configuration backup after verified recovery", 1
        )[1].split("- name: Remove the identity-bound temporary fixture", 1)[0]
        self.assertIn("steps.recovery.outcome == 'success'", private_cleanup)
        self.assertIn("config-before.yaml", private_cleanup)
        cleanup = block.split("- name: Remove the identity-bound temporary fixture", 1)[1]
        self.assertIn("if: always()", cleanup)
        self.assertIn('rm -f -- "$HIL_FIXTURE"', cleanup)

    def test_tag_runs_are_not_cancelled_while_hil_may_be_flashed(self):
        self.assertIn(
            "cancel-in-progress: ${{ !startsWith(github.ref, 'refs/tags/v') }}",
            WORKFLOW.split("jobs:", 1)[0],
        )

    def test_release_uses_the_verified_merged_image_builder(self):
        block = job("release")
        self.assertIn("bash /tmp/mkm5burner.sh", block)
        self.assertIn("MESHTASTIC_FACTORY=/tmp/fw.bin", block)
        self.assertNotIn("python -m esptool --chip esp32s3 merge_bin", block)

    def test_release_environment_installs_pillow_before_building_cover(self):
        block = job("release")
        install = block.index("pip install --require-hashes -r /tmp/release-requirements.txt")
        cover = block.index("python /tmp/mkcover.py")
        self.assertLess(install, cover)
        lock = (ROOT / "requirements/release.txt").read_text()
        self.assertRegex(lock, r"(?m)^pillow==12\.3\.0 .*sys_platform == 'linux'")

    def test_firmware_job_exercises_pixel_exact_cover_verification_with_pillow(self):
        block = job("firmware")
        install = block.index("python -m pip install --require-hashes -r hil/requirements.txt")
        cover_tests = block.index("tests.python.test_m5burner_publish")
        self.assertLess(install, cover_tests)

    def test_release_assets_remain_bound_to_tag_after_main_checkout(self):
        block = job("release")
        self.assertIn("fetch-depth: 0", block)
        self.assertIn(
            "git fetch --no-tags origin main:refs/remotes/origin/main", block
        )
        staging = block.index("- name: Stage exact tagged release inputs")
        draft = block.index("- name: Prepare or repair the draft GitHub release")
        m5burner = block.index("- name: Publish to M5Burner")
        checkout_main = block.index("git checkout -B main origin/main")
        publish = block.index("- name: Publish the GitHub release after every distribution gate")
        self.assertLess(staging, checkout_main)
        self.assertLess(draft, m5burner)
        self.assertLess(m5burner, checkout_main)
        self.assertLess(checkout_main, publish)
        self.assertIn("cp docs/unifont.bin /tmp/unifont.bin", block[:checkout_main])
        self.assertIn("cp scripts/check-installer.py /tmp/check-installer.py", block[:checkout_main])
        self.assertIn("cp scripts/mkm5burner.sh /tmp/mkm5burner.sh", block[:checkout_main])
        self.assertIn("cp scripts/verify_web_publish.py /tmp/verify_web_publish.py", block[:checkout_main])
        self.assertIn("cp scripts/verify_release_ref.py /tmp/verify_release_ref.py", block[:checkout_main])
        self.assertIn("cp scripts/demo_media.py /tmp/demo_media.py", block[:checkout_main])
        self.assertIn("cp scripts/update_social_image.py /tmp/update_social_image.py", block[:checkout_main])
        self.assertIn("cp docs/img/demo-story.json /tmp/demo-story.json", block[:checkout_main])
        self.assertIn("cp docs/img/mockup.json /tmp/mockup.json", block[:checkout_main])
        self.assertIn("cp docs/img/device.png /tmp/device.png", block[:checkout_main])
        self.assertIn("cp docs/m5burner-card.txt /tmp/m5burner-card.txt", block[:checkout_main])
        self.assertIn("cp requirements/release.txt /tmp/release-requirements.txt", block[:checkout_main])
        self.assertIn(
            'python scripts/release_notes.py "$GITHUB_REF_NAME" --output /tmp/release-notes.md',
            block[:checkout_main],
        )
        self.assertIn("pip install --require-hashes -r /tmp/release-requirements.txt", block[:checkout_main])
        self.assertIn('echo "engine=$engine" >> "$GITHUB_OUTPUT"', block[:checkout_main])
        self.assertIn("tag_commit=$(git rev-parse 'HEAD^{commit}')", block[:checkout_main])
        self.assertIn('test "$tag_commit" = "$GITHUB_SHA"', block[:checkout_main])
        self.assertIn('python scripts/release_tag.py "$GITHUB_REF_NAME" >> "$GITHUB_OUTPUT"', block[:checkout_main])
        self.assertIn("cp /tmp/unifont.bin docs/unifont.bin", block[m5burner:publish])
        self.assertIn('engine="${{ steps.tagged.outputs.engine }}"', block[staging:checkout_main])
        self.assertIn("python /tmp/check-installer.py --prune docs", block[m5burner:publish])
        self.assertIn("--manifest docs/manifest.json --archives docs/versions", block)
        self.assertNotIn("xargs -r rm -rf", block)
        self.assertIn("python /tmp/verify_web_publish.py", block[checkout_main:publish])
        self.assertIn("firmware=/tmp/fw.bin", block[checkout_main:publish])
        self.assertIn("font=/tmp/unifont.bin", block[checkout_main:publish])
        self.assertIn('--firmware "$firmware"', block[checkout_main:publish])
        self.assertIn('--font "$font"', block[checkout_main:publish])
        self.assertIn("MESHTASTIC_FONT=/tmp/unifont.bin", block[:draft])
        self.assertIn("python /tmp/m5burner_publish.py", block[draft:checkout_main])
        self.assertIn("--listing /tmp/m5burner-card.txt", block[draft:checkout_main])
        self.assertNotIn('--email "$M5B_EMAIL"', block[draft:checkout_main])
        self.assertGreaterEqual(block.count("python /tmp/verify_release_ref.py"), 4)

    def test_release_hil_publishes_only_sanitized_evidence_and_exact_media(self):
        physical = job("release-hil")
        self.assertIn("python scripts/demo_media.py build", physical)
        self.assertIn("--frames hil-artifacts/release-hil/usb/visual/frames", physical)
        self.assertIn("python scripts/demo_media.py stage-evidence", physical)
        upload = physical.split("- name: Upload privacy-safe behavioral HIL evidence", 1)[1]
        upload = upload.split("- name: Remove the identity-bound temporary fixture", 1)[0]
        self.assertIn("path: hil-artifacts/release-hil-public", upload)
        self.assertNotIn("path: hil-artifacts/release-hil\n", upload)
        self.assertNotIn("visual/frames", upload)

        release = job("release")
        download = release.index("- name: Download exact-tag privacy-safe HIL media")
        checkout_main = release.index("git checkout -B main origin/main")
        self.assertLess(download, checkout_main)
        verification = release.index("- name: Verify exact-tag HIL media before main checkout")
        self.assertLess(verification, checkout_main)
        self.assertIn("python /tmp/demo_media.py verify", release[verification:checkout_main])
        self.assertIn("release-hil-public/media/hero.gif", release)
        self.assertIn("release-hil-public/media/demo.gif", release)
        self.assertIn("meshtastic-adv-hero.gif meshtastic-adv-demo.gif", release)

    def test_only_stable_tags_update_public_site_media(self):
        block = job("release")
        installer = block.split("- name: Refresh or verify the installer image on main", 1)[1]
        installer = installer.split("- name: Verify public web installer and media", 1)[0]
        self.assertIn("if: steps.tagged.outputs.prerelease == 'false'", installer)
        self.assertIn("cp release-hil-public/media/hero.gif docs/img/hero.gif", installer)
        self.assertIn("cp release-hil-public/media/demo.gif docs/img/demo.gif", installer)
        self.assertIn("cp release-hil-public/media/hero.png docs/img/hero.png", installer)
        self.assertIn("cp release-hil-public/media/manifest.json docs/img/demo-manifest.json", installer)
        self.assertIn("python /tmp/update_social_image.py", installer)
        self.assertIn("docs/index.html", installer)
        verifier = block.split("- name: Verify public web installer and media", 1)[1]
        self.assertIn("hero=release-hil-public/media/hero.gif", verifier)
        self.assertIn("demo=release-hil-public/media/demo.gif", verifier)
        self.assertIn("poster=release-hil-public/media/hero.png", verifier)
        self.assertIn("demo_manifest=release-hil-public/media/manifest.json", verifier)
        self.assertIn("cover=/tmp/m5burner-cover.png", verifier)
        self.assertIn('--hero "$hero"', verifier)
        self.assertIn('--demo "$demo"', verifier)
        self.assertIn('--poster "$poster"', verifier)
        self.assertIn('--demo-manifest "$demo_manifest"', verifier)
        self.assertIn('--cover "$cover"', verifier)
        self.assertIn("--index /tmp/site-index.html", verifier)

    def test_stable_release_uses_exact_tag_changelog_notes_and_reruns_repair_them(self):
        block = job("release")
        staging = block.index("- name: Stage exact tagged release inputs")
        checkout_main = block.index("git checkout -B main origin/main")
        draft = block.index("- name: Prepare or repair the draft GitHub release")
        publish = block.index("- name: Publish the GitHub release after every distribution gate")
        self.assertLess(staging, draft)
        self.assertLess(draft, checkout_main)
        self.assertLess(checkout_main, publish)
        self.assertIn("notes_flags=(--generate-notes)", block[draft:checkout_main])
        self.assertIn("notes_flags=(--notes-file /tmp/release-notes.md)", block[draft:checkout_main])
        self.assertIn('gh release edit "$GITHUB_REF_NAME" --notes-file /tmp/release-notes.md', block[draft:checkout_main])
        self.assertIn("--draft --latest=false", block[draft:checkout_main])
        self.assertIn('gh release edit "$GITHUB_REF_NAME" --draft=false --latest=false', block[publish:])
        self.assertIn('gh release edit "$canonical_latest" --latest', block[publish:])

    def test_prerelease_cannot_replace_stable_distribution_channels(self):
        block = job("release")
        installer = block.split("- name: Refresh or verify the installer image on main", 1)[1]
        installer = installer.split("- name: Verify public web installer and media", 1)[0]
        draft = block.split("- name: Prepare or repair the draft GitHub release", 1)[1]
        m5burner = block.split("- name: Publish to M5Burner", 1)[1].split(
            "- name: Refresh or verify the installer image on main", 1
        )[0]
        release = block.split("- name: Publish the GitHub release after every distribution gate", 1)[1]
        self.assertIn("if: steps.tagged.outputs.prerelease == 'false'", installer)
        self.assertIn("release_flags+=(--prerelease)", draft)
        self.assertIn('"${release_flags[@]}"', draft)
        self.assertIn("--verify-tag", draft)
        self.assertIn("if: steps.tagged.outputs.prerelease == 'false'", m5burner)
        self.assertIn("--draft=false --prerelease --latest=false", release)
        local_publisher = (ROOT / "scripts/publish-firmware.sh").read_text()
        self.assertIn("invalid stable release version", local_publisher)
        self.assertNotIn("(?:[-+][0-9A-Za-z.-]+)?", local_publisher)

    def test_m5burner_publisher_never_sends_auth_over_plain_http(self):
        publisher = (ROOT / "scripts/m5burner_publish.py").read_text()
        self.assertIn('API = "https://m5burner-api.m5stack.com"', publisher)
        self.assertNotIn('API = "http://', publisher)

    def test_stable_release_fails_closed_until_m5burner_is_public(self):
        block = job("release")
        m5burner = block.split("- name: Publish to M5Burner", 1)[1].split(
            "- name: Refresh or verify the installer image on main", 1
        )[0]
        self.assertNotIn("continue-on-error", m5burner)
        self.assertNotIn("--check-listing-only", m5burner)
        self.assertIn('--version "${{ steps.tagged.outputs.version }}"', m5burner)
        self.assertIn("--listing /tmp/m5burner-card.txt", m5burner)
        self.assertIn('echo "M5Burner secrets not set"', m5burner)
        self.assertIn("exit 1", m5burner)
        self.assertNotIn("pip install", m5burner)
        publisher = (ROOT / "scripts/m5burner_publish.py").read_text()
        self.assertIn("verify_public_version", publisher)
        self.assertIn('public.get("published") is True', publisher)

    def test_public_github_release_is_the_last_distribution_marker(self):
        block = job("release")
        draft = block.index("- name: Prepare or repair the draft GitHub release")
        m5burner = block.index("- name: Publish to M5Burner")
        web = block.index("- name: Verify public web installer and media")
        publish = block.index("- name: Publish the GitHub release after every distribution gate")
        self.assertLess(draft, m5burner)
        self.assertLess(m5burner, web)
        self.assertLess(web, publish)
        final = block[publish:]
        self.assertGreaterEqual(final.count("python /tmp/verify_github_release.py"), 2)
        self.assertGreaterEqual(final.count('--title "Meshtastic ADV $GITHUB_REF_NAME"'), 2)
        self.assertIn("--tag \"$GITHUB_REF_NAME\" --state public", final)
        exact_public = final.index('--tag "$GITHUB_REF_NAME" --state public')
        canonical = final.index('canonical_latest="${{ steps.site.outputs.latest_stable_tag }}"')
        latest = final.index('gh release edit "$canonical_latest" --latest')
        self.assertLess(canonical, latest)
        self.assertLess(exact_public, latest)
        self.assertIn('releases/latest" --jq .tag_name', final)

    def test_historical_rerun_cannot_replace_github_latest(self):
        block = job("release")
        site = block.split("- name: Refresh or verify the installer image on main", 1)[1]
        site = site.split("- name: Verify public web installer and media", 1)[0]
        self.assertIn("--manifest docs/manifest.json --archives docs/versions", site)
        self.assertGreaterEqual(site.count("--retention-limit 6"), 2)
        self.assertIn('--output latest-tag', site)
        self.assertIn('if [ "$site_mode" = historical ]', site)
        self.assertIn('elif [ "$site_mode" = retired ]', site)
        self.assertIn("current root verified without mutation", site)

        publish = block.split("- name: Publish the GitHub release after every distribution gate", 1)[1]
        self.assertIn('canonical_latest="${{ steps.site.outputs.latest_stable_tag }}"', publish)
        self.assertIn('if [ "$GITHUB_REF_NAME" != "$canonical_latest" ]', publish)
        self.assertIn('gh release edit "$GITHUB_REF_NAME" --latest=false', publish)
        self.assertIn('gh release edit "$canonical_latest" --latest', publish)
        self.assertIn('= "$canonical_latest"', publish)

        canonical = publish.index(
            'canonical_latest="${{ steps.site.outputs.latest_stable_tag }}"'
        )
        precondition = publish.index(
            'canonical_state=$(gh release view "$canonical_latest"', canonical
        )
        make_public = publish.index('--draft=false --latest=false', precondition)
        exact_public = publish.index('--tag "$GITHUB_REF_NAME" --state public', make_public)
        latest = publish.index('gh release edit "$canonical_latest" --latest', exact_public)
        self.assertLess(canonical, precondition)
        self.assertLess(precondition, make_public)
        self.assertLess(make_public, exact_public)
        self.assertLess(exact_public, latest)

    def test_release_rechecks_main_ancestry_before_external_publication(self):
        block = job("release")
        m5 = block.split("- name: Publish to M5Burner", 1)[1].split(
            "- name: Refresh or verify the installer image on main", 1
        )[0]
        site = block.split("- name: Refresh or verify the installer image on main", 1)[1].split(
            "- name: Verify public web installer and media", 1
        )[0]
        publish = block.split(
            "- name: Publish the GitHub release after every distribution gate", 1
        )[1]
        for stage in (m5, site, publish):
            self.assertIn("git fetch --no-tags origin main:refs/remotes/origin/main", stage)
            self.assertIn("git merge-base --is-ancestor", stage)
            self.assertIn('refs/remotes/origin/main || {', stage)
            self.assertIn("release tag is no longer reachable from current main", stage)

    def test_external_actions_and_python_installs_are_immutable(self):
        for workflow in (WORKFLOW, REPAIR_WORKFLOW, FINISH_WORKFLOW):
            for reference in re.findall(r"(?m)^\s+uses:\s+([^\s#]+)", workflow):
                if reference.startswith("docker://"):
                    self.assertIn("@sha256:", reference)
                else:
                    self.assertRegex(reference, r"@[0-9a-f]{40}$")
            self.assertNotRegex(workflow, r"pip install ['\"]?[^\n]*[<>=]")
        self.assertEqual(WORKFLOW.count("pip install --require-hashes -r"), 3)
        self.assertEqual(WORKFLOW.count("pip install --require-hashes --no-deps"), 4)

    def test_m5burner_repair_is_exact_run_bound_and_cannot_mutate_github_release(self):
        self.assertIn("workflow_dispatch:", REPAIR_WORKFLOW)
        self.assertIn("actions: read", REPAIR_WORKFLOW)
        self.assertIn("contents: read", REPAIR_WORKFLOW)
        self.assertNotIn("contents: write", REPAIR_WORKFLOW)
        self.assertIn("group: meshtastic-adv-release-publish", REPAIR_WORKFLOW)
        self.assertIn('.path == ".github/workflows/build.yml"', REPAIR_WORKFLOW)
        self.assertIn('.head_sha == $sha', REPAIR_WORKFLOW)
        self.assertIn('.name == "release-hil" and .conclusion == "success"', REPAIR_WORKFLOW)
        self.assertGreaterEqual(REPAIR_WORKFLOW.count("run-id: ${{ inputs.source_run_id }}"), 2)
        self.assertIn('git archive "$REPAIR_TAG"', REPAIR_WORKFLOW)
        self.assertIn("python /tmp/tag-source/scripts/demo_media.py verify", REPAIR_WORKFLOW)
        self.assertIn("docs/img/device.png", REPAIR_WORKFLOW)
        self.assertLess(
            REPAIR_WORKFLOW.index("python -m pip install --require-hashes -r requirements/release.txt"),
            REPAIR_WORKFLOW.index("python /tmp/tag-source/scripts/mkcover.py"),
        )
        self.assertNotIn("gh release view", REPAIR_WORKFLOW)
        self.assertNotIn("gh release download", REPAIR_WORKFLOW)
        self.assertIn("MESHTASTIC_MERGED_OUT=/tmp/expected-merged.bin", REPAIR_WORKFLOW)
        self.assertIn("python scripts/m5burner_publish.py", REPAIR_WORKFLOW)
        self.assertNotIn("gh release edit", REPAIR_WORKFLOW)
        self.assertNotIn("gh release upload", REPAIR_WORKFLOW)

    def test_finish_workflow_resumes_only_an_exact_failed_stable_release(self):
        self.assertIn("workflow_dispatch:", FINISH_WORKFLOW)
        self.assertIn("actions: read", FINISH_WORKFLOW)
        self.assertIn("contents: write", FINISH_WORKFLOW)
        self.assertIn("group: meshtastic-adv-release-publish", FINISH_WORKFLOW)
        self.assertIn("ref: ${{ github.sha }}", FINISH_WORKFLOW)
        self.assertIn('test "$DISPATCH_REF" = refs/heads/main', FINISH_WORKFLOW)
        self.assertIn('test "$(git rev-parse HEAD)" = "$DISPATCH_SHA"', FINISH_WORKFLOW)
        self.assertGreaterEqual(
            FINISH_WORKFLOW.count(
                'test "$(git rev-parse refs/remotes/origin/main)" = "$DISPATCH_SHA"'
            ),
            3,
        )
        self.assertIn('.path == ".github/workflows/build.yml"', FINISH_WORKFLOW)
        self.assertIn('.head_sha == $sha', FINISH_WORKFLOW)
        self.assertIn('.name == "release-hil" and .conclusion == "success"', FINISH_WORKFLOW)
        self.assertGreaterEqual(FINISH_WORKFLOW.count("run-id: ${{ inputs.source_run_id }}"), 2)
        self.assertIn("promote|repair) ;;", FINISH_WORKFLOW)
        self.assertIn('git -C firmware show "$tag_firmware:version.properties"', FINISH_WORKFLOW)
        self.assertIn('.release_image_sha256', FINISH_WORKFLOW)
        self.assertIn('f[0x10000:0x10000+len(a)] != a', FINISH_WORKFLOW)
        self.assertIn('.summary.tests == (.cases | length)', FINISH_WORKFLOW)
        self.assertIn(
            '([.results[].name] | unique | length) == (.results | length)',
            FINISH_WORKFLOW,
        )
        self.assertIn('.data.frames == 35 and .data.unique == 35', FINISH_WORKFLOW)
        self.assertIn('test ! -e finish-hil/usb/visual/frames', FINISH_WORKFLOW)
        self.assertIn("python scripts/m5burner_publish.py", FINISH_WORKFLOW)
        self.assertGreaterEqual(
            FINISH_WORKFLOW.count("--verify-existing-latest-only"), 2
        )
        self.assertNotIn("M5B_EMAIL", FINISH_WORKFLOW)
        self.assertNotIn("M5B_PASSWORD", FINISH_WORKFLOW)
        self.assertNotIn("/tmp/tag-source/scripts/m5burner_publish.py", FINISH_WORKFLOW)
        self.assertNotIn("gh release create", FINISH_WORKFLOW)
        self.assertNotIn("gh release upload", FINISH_WORKFLOW)
        self.assertGreaterEqual(FINISH_WORKFLOW.count("--no-newer-stable"), 4)
        self.assertIn("git commit --allow-empty", FINISH_WORKFLOW)
        self.assertIn("steps.pages.outputs.site_commit", FINISH_WORKFLOW)
        self.assertGreaterEqual(FINISH_WORKFLOW.count("--versions-index"), 3)
        self.assertGreaterEqual(FINISH_WORKFLOW.count("--site-script"), 3)
        self.assertGreaterEqual(FINISH_WORKFLOW.count("--installer"), 3)
        self.assertGreaterEqual(FINISH_WORKFLOW.count("--web-tools-dir"), 3)
        self.assertGreaterEqual(FINISH_WORKFLOW.count("--archive-tag"), 2)
        m5 = FINISH_WORKFLOW.index("Verify the exact public M5Burner distribution")
        site = FINISH_WORKFLOW.index("Promote or repair the exact web installer on main")
        web = FINISH_WORKFLOW.index("Verify the exact public web installer and media")
        publish = FINISH_WORKFLOW.index(
            "Publish the exact GitHub draft as the final distribution marker"
        )
        self.assertLess(m5, site)
        self.assertLess(site, web)
        self.assertLess(web, publish)
        final = FINISH_WORKFLOW[publish:]
        first_edit = final.index('gh release edit "$FINISH_TAG" --draft=false')
        self.assertLess(final.index("--attempts 1 --delay 0"), first_edit)
        self.assertLess(final.index("--verify-existing-latest-only"), first_edit)
        self.assertLess(final.index("steps.pages.outputs.site_commit"), first_edit)

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

    def test_versioned_installer_font_is_not_ignored(self):
        ignore = (ROOT / ".gitignore").read_text()
        self.assertIn("!docs/versions/*/unifont.bin", ignore)
        for workflow in (WORKFLOW, FINISH_WORKFLOW):
            self.assertRegex(
                workflow,
                r'cp (?:/tmp/)?unifont\.bin "?docs/versions/',
            )

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
