#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Writable-field SYSREG write tests and Table 2 report.

Requires inventory JSON from idregs_inventory.py (--collect).

Test expectations (--gen-unittests) are derived from Linux
arch/arm64/kernel/cpufeature.c ftr_bits (FTR_LOWER_SAFE, etc.) plus the
Guest MRS host value.

--run executes those tests and fills Table 2 (--report) from PASS/FAIL.
--probe remains as an optional empirical alternative (not used for expectations).

See arm/idregs-README.txt.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from idregs_lib import (  # noqa: E402
    DEFAULT_JSON,
    DEFAULT_PROBE_HTML,
    INC,
    REPO,
    die,
    generate_unittests,
    parse_named_fields,
    parse_run_logs,
    parse_run_stdout,
    probe_writable_values,
    run_kut_group,
    shutil_which,
    write_probe_report,
)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--qemu",
        default=os.environ.get("QEMU", "qemu-system-aarch64"),
        help="QEMU binary (or set $QEMU)",
    )
    ap.add_argument("--json", type=Path, default=DEFAULT_JSON)
    ap.add_argument("--html", type=Path, default=DEFAULT_PROBE_HTML)
    ap.add_argument(
        "--probe",
        action="store_true",
        help="Optional legacy: empirical probe (not used for expectations)",
    )
    ap.add_argument(
        "--gen-unittests",
        action="store_true",
        help="Regenerate write tests from Linux ftr policy + inventory host MRS",
    )
    ap.add_argument(
        "--report",
        action="store_true",
        help="Write Table 2 HTML from --json (filled by --run, or legacy --probe)",
    )
    ap.add_argument(
        "--run",
        action="store_true",
        help="Run ./run_tests.sh -g idregs; store results and write Table 2 HTML",
    )
    ap.add_argument(
        "--run-mismatch",
        action="store_true",
        help="Also run -g idregs-mismatch",
    )
    ap.add_argument("-j", "--parallel", type=int, default=1)
    ap.add_argument(
        "--all",
        action="store_true",
        help="--gen-unittests --run (includes Table 2 report)",
    )
    args = ap.parse_args()
    if args.all:
        args.gen_unittests = args.run = True
    if not any(
        [args.probe, args.gen_unittests, args.report, args.run, args.run_mismatch]
    ):
        ap.print_help()
        return 2

    qemu = args.qemu
    if not args.json.is_file():
        die(f"missing {args.json}; run scripts/idregs_inventory.py --collect first")

    data = json.loads(args.json.read_text())
    if not data.get("inventory"):
        die(f"{args.json} has no inventory; run idregs_inventory.py --collect")

    if args.probe:
        if not Path(qemu).is_file() and not shutil_which(qemu):
            die(f"QEMU not found: {qemu}")
        if not INC.is_file():
            die(f"missing {INC}")
        named = parse_named_fields(INC.read_text())
        props = set(data.get("available_props") or [])
        if not props:
            props = {
                e["property"]
                for e in data["inventory"]
                if e.get("writable") and e.get("property")
            }
        qom_host = {}
        guest_host = {}
        for e in data["inventory"]:
            key = (e["register"], e["field"])
            if e.get("host") and e["host"] != "?":
                try:
                    guest_host[key] = int(e["host"], 0)
                except ValueError:
                    guest_host[key] = None
            if e.get("writable") and e.get("qom_host"):
                try:
                    qom_host[e["property"]] = int(e["qom_host"], 0)
                except (ValueError, TypeError):
                    pass
        os.chdir(REPO)
        print("Probing writable SYSREG values (legacy)...", flush=True)
        results, unexpected = probe_writable_values(
            qemu, named, props, qom_host, guest_host
        )
        data["results"] = results
        data["unexpected"] = unexpected
        data["results_source"] = "probe"
        data["run_counts"] = {}
        data["generated"] = datetime.now(timezone.utc).astimezone().isoformat(
            timespec="seconds"
        )
        data["qemu"] = qemu
        if not data.get("host_name"):
            data["host_name"] = os.uname().nodename
        args.json.write_text(json.dumps(data, indent=2) + "\n")
        print(
            f"Wrote {args.json}: results={len(results)} unexpected={len(unexpected)}",
            flush=True,
        )
        write_probe_report(data, args.html)
        print(f"Wrote {args.html}", flush=True)

    if args.gen_unittests:
        generate_unittests(data["inventory"], data.get("results") or [])

    rc = 0
    if args.run:
        flat = REPO / "arm" / "idregs.flat"
        if not flat.is_file():
            print("Building arm/idregs.flat …", flush=True)
            subprocess.check_call(
                ["make", "arm/idregs.flat", f"-j{os.cpu_count() or 4}"],
                cwd=REPO,
            )
        os.chdir(REPO)
        run_rc, out = run_kut_group(qemu, "idregs", args.parallel, capture=True)
        rc = run_rc or rc
        results, unexpected, counts = parse_run_stdout(out, data["inventory"])
        data["results"] = results
        data["unexpected"] = unexpected
        data["run_counts"] = counts
        data["results_source"] = "run"
        data["generated"] = datetime.now(timezone.utc).astimezone().isoformat(
            timespec="seconds"
        )
        data["qemu"] = qemu
        if not data.get("host_name"):
            data["host_name"] = os.uname().nodename
        args.json.write_text(json.dumps(data, indent=2) + "\n")
        print(
            f"Wrote {args.json}: fields={len(results)} unexpected={len(unexpected)} "
            f"PASS={counts.get('pass', 0)} FAIL={counts.get('fail', 0)} "
            f"SKIP={counts.get('skip', 0)}",
            flush=True,
        )
        write_probe_report(data, args.html)
        print(f"Wrote {args.html}", flush=True)

    if args.run_mismatch:
        run_rc, _out = run_kut_group(
            qemu, "idregs-mismatch", args.parallel, capture=False
        )
        rc = run_rc or rc

    # Standalone --report (re-render from JSON); skipped if --run/--probe already wrote it
    if args.report and not args.run and not args.probe:
        if not data.get("results"):
            log_dir = REPO / "logs"
            if log_dir.is_dir() and any(log_dir.glob("idregs-*.log")):
                print(f"Rebuilding Table 2 from {log_dir} …", flush=True)
                results, unexpected, counts = parse_run_logs(
                    log_dir, data["inventory"]
                )
                data["results"] = results
                data["unexpected"] = unexpected
                data["run_counts"] = counts
                data["results_source"] = "run"
                data["generated"] = datetime.now(timezone.utc).astimezone().isoformat(
                    timespec="seconds"
                )
                if not data.get("host_name"):
                    data["host_name"] = os.uname().nodename
                args.json.write_text(json.dumps(data, indent=2) + "\n")
                print(
                    f"Wrote {args.json}: fields={len(results)} "
                    f"PASS={counts.get('pass', 0)} FAIL={counts.get('fail', 0)}",
                    flush=True,
                )
        write_probe_report(data, args.html)
        print(f"Wrote {args.html}", flush=True)

    return rc


if __name__ == "__main__":
    raise SystemExit(main())
