#!/usr/bin/env python3
"""Run the reproducible ADV verification pipeline, with hardware opt-in only."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


@dataclass(frozen=True)
class Stage:
    name: str
    command: tuple[str, ...]
    hardware: bool = False


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    mode = result.add_mutually_exclusive_group()
    mode.add_argument("--quick", action="store_true", help="run syntax, unit and sanitizer checks only")
    mode.add_argument("--firmware-only", action="store_true", help="run firmware builds and size checks only")
    mode.add_argument(
        "--hardware-only", action="store_true",
        help="run only opted-in hardware stages against already verified build artifacts",
    )
    result.add_argument("--skip-build", action="store_true", help="validate already-built firmware artifacts")
    result.add_argument("--rf", action="store_true", help="opt in to the state-preserving two-node RF suite")
    result.add_argument("--usb", action="store_true", help="opt in to the Cardputer USB HIL suite")
    result.add_argument(
        "--production-wifi", action="store_true",
        help="after USB HIL, soak the restored exact production image through read-only PhoneAPI dumps",
    )
    result.add_argument(
        "--release-image", type=Path,
        help="exact production app artifact restored after USB HIL (requires --usb)",
    )
    result.add_argument(
        "--config-backup", type=Path,
        help="exclusive private backup destination for USB HIL (requires --usb)",
    )
    result.add_argument("--backup-root", type=Path, help="pre-run WiFi fixture backup root required by --rf")
    result.add_argument("--capture-rf-backups", action="store_true", help="capture fresh WiFi exports before --rf")
    result.add_argument("--rf-profile-from", help="fixture name that supplies the intentional shared RF profile")
    result.add_argument("--rf-expect-region", help="required region name for the selected RF profile")
    result.add_argument("--rf-expect-tx-power", type=int, help="required dBm for the selected RF profile")
    result.add_argument("--fixture", type=Path, default=ROOT / "hil/fixture.local.json")
    result.add_argument("--artifacts", type=Path)
    result.add_argument("--timeout", type=float, default=240)
    return result


def validate_args(args: argparse.Namespace, source: argparse.ArgumentParser) -> None:
    if (args.quick or args.firmware_only) and (args.rf or args.usb):
        source.error("hardware stages cannot be combined with --quick or --firmware-only")
    if args.hardware_only and not (args.rf or args.usb):
        source.error("--hardware-only requires --usb or --rf")
    if args.hardware_only and not args.skip_build:
        source.error("--hardware-only requires --skip-build")
    if args.release_image is not None and not args.usb:
        source.error("--release-image requires --usb")
    if args.config_backup is not None and not args.usb:
        source.error("--config-backup requires --usb")
    if args.production_wifi and not args.usb:
        source.error("--production-wifi requires --usb")
    if args.production_wifi and args.release_image is None:
        source.error("--production-wifi requires --release-image")
    if args.rf and args.backup_root is None:
        source.error("--rf requires --backup-root")
    if (
        args.capture_rf_backups
        or args.rf_profile_from
        or args.rf_expect_region
        or args.rf_expect_tx_power is not None
    ) and not args.rf:
        source.error("RF backup/profile assertions require --rf")
    if args.quick and args.skip_build:
        source.error("--skip-build has no effect with --quick")


def build_plan(args: argparse.Namespace, artifacts: Path) -> list[Stage]:
    python = sys.executable
    shell_scripts = tuple(str(path) for path in sorted((ROOT / "scripts").glob("*.sh")))
    python_sources = tuple(
        str(path)
        for directory in (ROOT / "scripts", ROOT / "tests/python")
        for path in sorted(directory.glob("*.py"))
    )
    stages: list[Stage] = []

    if not args.firmware_only and not args.hardware_only:
        stages.extend(
            [
                Stage("syntax/python", (python, "-m", "py_compile", *python_sources)),
                Stage("syntax/shell", ("bash", "-n", *shell_scripts)),
                Stage(
                    "host/unit",
                    (python, "-m", "unittest", "discover", "-s", "tests/python", "-p", "test_*.py", "-v"),
                ),
                Stage("host/sanitizers", ("bash", "scripts/test-host.sh")),
            ]
        )

    if not args.quick and not args.hardware_only:
        if not args.skip_build:
            # Stage the recovery image last before hardware is allowed to run.
            stages.extend(
                [
                    Stage("firmware/build-hil", (python, "scripts/hil.py", "build")),
                    Stage("firmware/build-release", (python, "scripts/hil.py", "build-release")),
                ]
            )
        stages.extend(
            [
                Stage(
                    "firmware/size-release",
                    (python, "scripts/check-size.py", "firmware/.pio/build/m5stack-cardputer-adv-advui"),
                ),
                Stage(
                    "firmware/size-hil",
                    (python, "scripts/check-size.py", "firmware/.pio/build/m5stack-cardputer-adv-advui-hil"),
                ),
            ]
        )

    # Run deterministic USB ingress/UI/storage coverage before the RF stage: a
    # weak or temporarily shadowed antenna path must not hide useful HIL evidence.
    if args.usb:
        usb_command = [
            python,
            "scripts/hil.py",
            "run",
            "--fixture",
            str(args.fixture),
            "--artifacts",
            str(artifacts / "usb"),
            "--timeout",
            str(args.timeout),
            "--skip-build",
        ]
        if args.release_image is not None:
            usb_command.extend(("--release-image", str(args.release_image)))
        if args.config_backup is not None:
            usb_command.extend(("--config-backup", str(args.config_backup)))
        if args.production_wifi:
            usb_command.append("--production-wifi")
        stages.append(Stage("hardware/usb-cardputer", tuple(usb_command), hardware=True))
    if args.rf:
        rf_command = [
            python,
            "scripts/rf_hil.py",
            "--fixture",
            str(args.fixture),
            "--backup-root",
            str(args.backup_root),
            "--artifacts",
            str(artifacts / "rf"),
            "--timeout",
            str(args.timeout),
        ]
        if args.capture_rf_backups:
            rf_command.append("--capture-backups")
        if args.rf_profile_from:
            rf_command.extend(("--profile-from", args.rf_profile_from))
        if args.rf_expect_region:
            rf_command.extend(("--expect-region", args.rf_expect_region))
        if args.rf_expect_tx_power is not None:
            rf_command.extend(("--expect-tx-power", str(args.rf_expect_tx_power)))
        stages.append(
            Stage(
                "hardware/rf-private",
                tuple(rf_command),
                hardware=True,
            )
        )
    return stages


def safe_log_name(name: str) -> str:
    return name.replace("/", "-") + ".log"


def run_stage(stage: Stage, log_path: Path) -> tuple[int, float]:
    started = time.monotonic()
    print(f"\n==> {stage.name}", flush=True)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("w", encoding="utf-8") as log:
        os.chmod(log_path, 0o600)
        try:
            process = subprocess.Popen(
                stage.command,
                cwd=ROOT,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                bufsize=1,
            )
        except OSError as exc:
            message = f"could not start stage: {type(exc).__name__}: {exc}\n"
            print(message, end="", file=sys.stderr)
            log.write(message)
            return 127, time.monotonic() - started
        assert process.stdout is not None
        for line in process.stdout:
            print(line, end="")
            log.write(line)
        return process.wait(), time.monotonic() - started


def write_report(report: dict[str, object], artifacts: Path) -> None:
    artifacts.mkdir(parents=True, exist_ok=True)
    json_path = artifacts / "report.json"
    json_path.write_text(json.dumps(report, indent=2) + "\n")
    os.chmod(json_path, 0o600)

    results = report["results"]
    assert isinstance(results, list)
    failures = sum(item.get("status") == "failed" for item in results if isinstance(item, dict))
    suite = ET.Element(
        "testsuite",
        name="meshtastic-adv-verification",
        tests=str(len(results)),
        failures=str(failures),
    )
    for item in results:
        if not isinstance(item, dict):
            continue
        case = ET.SubElement(
            suite,
            "testcase",
            name=str(item["name"]),
            time=f"{float(item['duration_seconds']):.3f}",
        )
        if item.get("status") == "failed":
            message = f"stage exited with code {item['returncode']}"
            ET.SubElement(case, "failure", message=message).text = message
    junit_path = artifacts / "junit.xml"
    ET.ElementTree(suite).write(junit_path, encoding="utf-8", xml_declaration=True)
    os.chmod(junit_path, 0o600)


def main(argv: list[str] | None = None) -> int:
    source = parser()
    args = source.parse_args(argv)
    validate_args(args, source)
    artifacts = args.artifacts or ROOT / "hil-artifacts" / f"{datetime.now():%Y%m%d-%H%M%S}-verify"
    plan = build_plan(args, artifacts)
    report: dict[str, object] = {
        "suite": "meshtastic-adv-verification",
        "started": datetime.now(timezone.utc).isoformat(),
        "status": "running",
        "planned": len(plan),
        "hardware_opt_in": {
            "rf": args.rf,
            "usb": args.usb,
            "production_wifi": args.production_wifi,
        },
        "results": [],
    }
    write_report(report, artifacts)

    for stage in plan:
        returncode, duration = run_stage(stage, artifacts / "logs" / safe_log_name(stage.name))
        result = {
            "name": stage.name,
            "hardware": stage.hardware,
            "status": "passed" if returncode == 0 else "failed",
            "returncode": returncode,
            "duration_seconds": round(duration, 3),
        }
        report["results"].append(result)
        if returncode != 0:
            report["status"] = "failed"
            write_report(report, artifacts)
            print(f"\nVERIFY FAILED: {stage.name}; artifacts: {artifacts}", file=sys.stderr)
            return 1
        write_report(report, artifacts)

    report["status"] = "passed"
    report["finished"] = datetime.now(timezone.utc).isoformat()
    write_report(report, artifacts)
    print(f"\nVERIFY PASSED: {len(plan)} stages; artifacts: {artifacts}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
