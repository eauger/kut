#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Collect ID-reg field writability and host defaults (QOM + Guest MRS).

Does NOT probe writing new SYSREG values — see idregs_write_probe.py.

Writes scripts/idregs/idregs-sysreg-probe.json (inventory section).
Optionally regenerates writable-field write tests (*-host), runs idregs-host,
or writes Table 1 HTML (--report → idregs-sysreg-inventory.html).

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
    DEFAULT_INVENTORY_HTML,
    DEFAULT_JSON,
    INC,
    REPO,
    build_inventory,
    die,
    enrich_inventory_ftr_policy,
    generate_unittests,
    guest_host_defaults,
    parse_named_fields,
    host_type_name,
    qmp_list_props_and_host,
    run_kut_group,
    shutil_which,
    write_inventory_report,
)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--qemu",
        default=os.environ.get("QEMU", "qemu-system-aarch64"),
        help="QEMU binary (or set $QEMU)",
    )
    ap.add_argument("--json", type=Path, default=DEFAULT_JSON)
    ap.add_argument(
        "--collect",
        action="store_true",
        help="QMP writability + QOM (writable) + Guest MRS (all fields)",
    )
    ap.add_argument(
        "--gen-unittests",
        action="store_true",
        help="Regenerate writable write tests (Linux ftr policy + host MRS)",
    )
    ap.add_argument(
        "--run",
        action="store_true",
        help="Run idregs-host (writable write tests; needs write-probe results)",
    )
    ap.add_argument(
        "--report",
        action="store_true",
        help="Write Table 1 HTML (inventory) from --json",
    )
    ap.add_argument("--html", type=Path, default=DEFAULT_INVENTORY_HTML)
    ap.add_argument("-j", "--parallel", type=int, default=1)
    ap.add_argument(
        "--all",
        action="store_true",
        help="--collect --gen-unittests --run",
    )
    args = ap.parse_args()
    if args.all:
        args.collect = args.gen_unittests = args.run = True
    if not any([args.collect, args.gen_unittests, args.run, args.report]):
        ap.print_help()
        return 2

    qemu = args.qemu
    data = None

    if args.collect:
        if not Path(qemu).is_file() and not shutil_which(qemu):
            die(f"QEMU not found: {qemu}")
        if not INC.is_file():
            die(f"missing {INC}")
        named = parse_named_fields(INC.read_text())
        print(f"Named fields: {len(named)}", flush=True)
        props, qom_host = qmp_list_props_and_host(qemu)
        print(f"Exposed SYSREG props: {len(props)}", flush=True)
        os.chdir(REPO)
        print("Reading guest MRS for all named fields (single dump boot)...", flush=True)
        guest_host = guest_host_defaults(qemu, named)
        inventory = build_inventory(named, props, qom_host, guest_host)
        prev = {}
        if args.json.is_file():
            try:
                prev = json.loads(args.json.read_text())
            except json.JSONDecodeError:
                prev = {}
        data = {
            "host_name": host_type_name(),
            "qemu": qemu,
            "generated": datetime.now(timezone.utc).astimezone().isoformat(
                timespec="seconds"
            ),
            "inventory": inventory,
            "results": prev.get("results") or [],
            "unexpected": prev.get("unexpected") or [],
            "available_props": sorted(props),
        }
        inv_host = {(e["register"], e["field"]): e["host"] for e in inventory}
        for r in data["results"]:
            hv = inv_host.get((r["register"], r["field"]))
            if hv:
                r["host"] = hv
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(data, indent=2) + '\n')
        print(
            f"Wrote {args.json}: named={len(inventory)} "
            f"writable={sum(1 for e in inventory if e['writable'])} "
            f"with_mrs={sum(1 for e in inventory if e.get('host'))}",
            flush=True,
        )

    if args.gen_unittests or args.run or args.report:
        if data is None:
            if not args.json.is_file():
                die(f"missing {args.json}; run with --collect first")
            data = json.loads(args.json.read_text())

    if args.gen_unittests:
        assert data
        generate_unittests(data["inventory"], data.get("results") or [])

    if args.report:
        assert data
        enrich_inventory_ftr_policy(data["inventory"])
        args.json.write_text(json.dumps(data, indent=2) + "\n")
        write_inventory_report(data, args.html)
        print(f"Wrote {args.html}", flush=True)

    rc = 0
    if args.run:
        flat = REPO / "arm" / "idregs.flat"
        if not flat.is_file():
            print("Building arm/idregs.flat …", flush=True)
            subprocess.check_call(
                ["make", "arm/idregs.flat", f"-j{os.cpu_count() or 4}"],
                cwd=REPO,
            )
        rc = run_kut_group(qemu, "idregs-host", args.parallel) or rc
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
