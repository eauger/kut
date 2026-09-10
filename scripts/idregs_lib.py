#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Shared helpers for ID-register SYSREG inventory and write-probe scripts."""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from collections import OrderedDict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from idregs_ftr_policy import (  # noqa: E402
    classify_write_expectation,
    default_linux_cpufeature,
    lookup_ftr_policy,
    parse_linux_ftr_policies,
)

REPO = Path(__file__).resolve().parents[1]
INC = REPO / "arm" / "cpu-idregs.h.inc"
UNITTESTS = REPO / "arm" / "unittests.cfg"
DEFAULT_JSON = REPO / "scripts" / "idregs" / "idregs-sysreg-probe.json"
DEFAULT_HTML = REPO / "scripts" / "idregs" / "idregs-sysreg-report.html"  # legacy combined
DEFAULT_INVENTORY_HTML = REPO / "scripts" / "idregs" / "idregs-sysreg-inventory.html"
DEFAULT_PROBE_HTML = REPO / "scripts" / "idregs" / "idregs-sysreg-probe-report.html"
RESERVED = frozenset({"RES0", "RES1", "RAZ"})



def host_type_name(nodename: str | None = None) -> str:
    """Short machine type for reports, e.g. grace-hopper (not FQDN/instance)."""
    name = (nodename or os.uname().nodename).split(".")[0]
    name = re.sub(r"-\d+$", "", name)
    for prefix in ("nvidia-", "ampere-", "qualcomm-", "marvell-"):
        if name.lower().startswith(prefix):
            name = name[len(prefix) :]
            break
    return name or "unknown"

def die(msg: str, code: int = 1) -> None:
    print(msg, file=sys.stderr)
    raise SystemExit(code)


def parse_named_fields(text: str) -> list[dict[str, Any]]:
    """Parse named fields from cpu-idregs.h.inc (excludes reserved)."""
    fields: list[dict[str, Any]] = []
    for m in re.finditer(
        r"IDREG_FIELD_ENUM\(([^,]+),\s*([^,]+),\s*(\d+),\s*(\d+),([\s\S]*?)\n\s*\)",
        text,
    ):
        field = m.group(2).strip()
        if field in RESERVED:
            continue
        ranges = [
            (int(a), int(b))
            for a, b in re.findall(
                r"IDREG_ARCH_VALUE_RANGE\((\d+),\s*(\d+)\)", m.group(5)
            )
        ]
        fields.append(
            {
                "reg": m.group(1).strip(),
                "field": field,
                "shift": int(m.group(3)),
                "width": int(m.group(4)),
                "ranges": ranges,  # may be empty list
                "has_arch_ranges": bool(ranges),
            }
        )
    for m in re.finditer(
        r"IDREG_FIELD\(([^,]+),\s*([^,]+),\s*(\d+),\s*(\d+)\)", text
    ):
        field = m.group(2).strip()
        if field in RESERVED:
            continue
        fields.append(
            {
                "reg": m.group(1).strip(),
                "field": field,
                "shift": int(m.group(3)),
                "width": int(m.group(4)),
                "ranges": None,
                "has_arch_ranges": False,
            }
        )
    # stable unique by reg.field (ENUM wins if duplicate — shouldn't happen)
    seen: dict[tuple[str, str], dict[str, Any]] = {}
    for f in fields:
        seen[(f["reg"], f["field"])] = f
    return list(seen.values())


def prop_name(reg: str, field: str) -> str:
    return f"SYSREG_{reg}_{field}"


def expand_probe_values(f: dict[str, Any]) -> tuple[list[int], int | None, str]:
    """Return (values_to_probe, oor_value_or_None, arch_ranges_display)."""
    width = f["width"]
    maxv = (1 << width) - 1
    if f["has_arch_ranges"]:
        vals: set[int] = set()
        for lo, hi in f["ranges"]:
            vals.update(range(lo, hi + 1))
        arch = sorted(vals)
        oor = next((v for v in range(maxv + 1) if v not in vals), None)
        disp = ",".join(
            f"{a}-{b}" if a != b else str(a) for a, b in f["ranges"]
        )
        return arch, oor, disp
    # No enum in header
    if width <= 4:
        return list(range(maxv + 1)), None, ""
    # wide plain: sample only (caller adds host)
    return [], None, ""


def qmp_list_props_and_host(qemu: str) -> tuple[set[str], dict[str, int | None]]:
    p = subprocess.Popen(
        [
            qemu,
            "-accel",
            "kvm",
            "-cpu",
            "host",
            "-machine",
            "virt,gic-version=host",
            "-display",
            "none",
            "-S",
            "-qmp",
            "stdio",
        ],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    def send(obj: dict[str, Any]) -> None:
        assert p.stdin
        p.stdin.write(json.dumps(obj) + "\n")
        p.stdin.flush()

    def recv() -> dict[str, Any] | None:
        assert p.stdout
        while True:
            line = p.stdout.readline()
            if not line:
                return None
            o = json.loads(line)
            if "event" in o:
                continue
            return o

    recv()
    send({"execute": "qmp_capabilities"})
    recv()
    path = "/machine/unattached/device[0]"
    send({"execute": "qom-list", "arguments": {"path": path}})
    r = recv() or {}
    props = {
        x["name"]
        for x in (r.get("return") or [])
        if str(x.get("name", "")).startswith("SYSREG_")
    }
    host: dict[str, int | None] = {}
    for pr in sorted(props):
        send({"execute": "qom-get", "arguments": {"path": path, "property": pr}})
        rr = recv() or {}
        val = rr.get("return")
        host[pr] = int(val) if isinstance(val, int) else None
    send({"execute": "quit"})
    try:
        recv()
    except Exception:
        pass
    p.wait(timeout=10)
    return props, host


def run_cmd(
    cmd: list[str], timeout: float = 30, env: dict[str, str] | None = None
) -> tuple[int, str]:
    e = os.environ.copy()
    if env:
        e.update(env)
    p = subprocess.run(
        cmd, capture_output=True, text=True, timeout=timeout, env=e
    )
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def try_set(qemu: str, reg: str, field: str, value: int) -> tuple[bool, str, str]:
    prop = f"{prop_name(reg, field)}={value:#x}"
    rc, out = run_cmd(
        [
            "timeout",
            "0.4s",
            qemu,
            "-nodefaults",
            "-machine",
            "virt,gic-version=host",
            "-accel",
            "kvm",
            "-cpu",
            f"host,{prop}",
            "-display",
            "none",
            "-serial",
            "none",
            "-monitor",
            "none",
            "-S",
        ],
        timeout=5,
    )
    err = " ".join(out.split())
    started = rc in (0, 124) and not any(
        k in err
        for k in (
            "can't apply",
            "fail to apply",
            "whereas pmu",
            "exceeds length",
            "does not match any arch",
            "not found",
            "Inconsistent",
        )
    )
    if started:
        return True, "", err
    if "not found" in err:
        layer = "no property"
    elif "does not match any arch" in err:
        layer = "out of arch range"
    elif "whereas pmu" in err or "Inconsistent" in err:
        layer = "QEMU policy"
    elif "fail to apply" in err or "Invalid argument" in err:
        layer = "KVM"
    elif "exceeds length" in err:
        layer = "QEMU width"
    else:
        layer = "other"
    return False, layer, err


def guest_check(
    qemu: str, reg: str, field: str, value: int, apply_sysreg: bool = True
) -> tuple[int | None, int | None, str | None]:
    prop = f"{prop_name(reg, field)}={value:#x}"
    cpu = f"host,{prop}" if apply_sysreg else "host"
    rc, out = run_cmd(
        [
            str(REPO / "arm-run"),
            "arm/idregs.flat",
            "-append",
            prop,
            "-cpu",
            cpu,
        ],
        timeout=40,
        env={"QEMU": qemu},
    )
    m = re.search(
        r"(?:PASS|FAIL): idregs: ([^.]+)\.([^:]+): "
        r"expected=0x([0-9a-fA-F]+) mrs=0x([0-9a-fA-F]+)",
        out,
    )
    if not m:
        if "assert failed" in out or "STATUS=127" in out:
            return None, None, "guest abort"
        return None, None, "no guest report"
    return int(m.group(3), 16), int(m.group(4), 16), None



def guest_read_raw(qemu: str, reg: str, field: str) -> tuple[int | None, str | None]:
    """Boot guest with -cpu host; return (raw_sysreg, error).

    Legacy helper (one boot). Prefer guest_host_defaults() which dumps all
    registers in a single boot via test_args=dump.
    """
    prop = f"{prop_name(reg, field)}=0x0"
    _rc, out = run_cmd(
        [
            str(REPO / "arm-run"),
            "arm/idregs.flat",
            "-append",
            prop,
            "-cpu",
            "host",
        ],
        timeout=40,
        env={"QEMU": qemu},
    )
    m = re.search(
        r"(?:PASS|FAIL): idregs: ([^.]+)\.([^:]+): "
        r"expected=0x([0-9a-fA-F]+) mrs=0x([0-9a-fA-F]+)"
        r"(?: \(raw=0x([0-9a-fA-F]+)\))?",
        out,
    )
    if not m:
        if "MRS not implemented" in out:
            return None, "MRS not implemented"
        if "assert failed" in out or "STATUS=127" in out:
            return None, "guest abort"
        if "unknown SYSREG mapping" in out:
            return None, "unknown SYSREG mapping"
        return None, "no guest report"
    raw_s = m.group(5)
    if raw_s is not None:
        return int(raw_s, 16), None
    return None, "no raw in report"


def guest_host_defaults(
    qemu: str, named: list[dict[str, Any]]
) -> dict[tuple[str, str], int | None]:
    """Guest MRS for every named field with plain -cpu host (no SYSREG).

    Single guest boot: arm/idregs.flat with test_args=dump prints all
    IDREGS_DUMP lines; we parse FIELD= values (and fall back to RAW+shift).
    """
    flat = REPO / "arm" / "idregs.flat"
    if not flat.is_file():
        print("Building arm/idregs.flat …", flush=True)
        subprocess.check_call(
            ["make", "arm/idregs.flat", f"-j{os.cpu_count() or 4}"],
            cwd=REPO,
        )

    print("Reading guest MRS for all named fields (one boot, dump)…", flush=True)
    _rc, out = run_cmd(
        [
            str(REPO / "arm-run"),
            "arm/idregs.flat",
            "-append",
            "dump",
            "-cpu",
            "host",
        ],
        timeout=120,
        env={"QEMU": qemu},
    )
    if "IDREGS_DUMP_BEGIN" not in out:
        die(
            "idregs dump failed (no IDREGS_DUMP_BEGIN). "
            "Rebuild arm/idregs.flat and ensure QEMU works. Output tail:\n"
            + "\n".join(out.splitlines()[-30:])
        )

    field_vals: dict[tuple[str, str], int] = {}
    raw_by_reg: dict[str, int] = {}
    fail_regs: dict[str, str] = {}
    for line in out.splitlines():
        if not line.startswith("IDREGS_DUMP "):
            continue
        body = line[len("IDREGS_DUMP ") :]
        if body.startswith("REG=") and " RAW=" in body:
            # REG=CTR_EL0 RAW=0xb444c004
            rm = re.match(r"REG=(\S+) RAW=(0x[0-9a-fA-F]+)", body)
            if rm:
                raw_by_reg[rm.group(1)] = int(rm.group(2), 0)
                print(f"guest-host {rm.group(1)} raw={int(rm.group(2), 0):#x}", flush=True)
        elif body.startswith("REG=") and " FAIL=" in body:
            rm = re.match(r"REG=(\S+) FAIL=(\S+)", body)
            if rm:
                fail_regs[rm.group(1)] = rm.group(2)
                print(
                    f"guest-host {rm.group(1)}.* FAILED: {rm.group(2)}",
                    flush=True,
                )
        elif body.startswith("FIELD="):
            fm = re.match(
                r"FIELD=([^.]+)\.(\S+) VAL=(0x[0-9a-fA-F]+)", body
            )
            if fm:
                field_vals[(fm.group(1), fm.group(2))] = int(fm.group(3), 0)

    out_map: dict[tuple[str, str], int | None] = {}
    for f in named:
        key = (f["reg"], f["field"])
        if key in field_vals:
            out_map[key] = field_vals[key]
            print(
                f"  {f['reg']}.{f['field']} = {field_vals[key]:#x}",
                flush=True,
            )
            continue
        if f["reg"] in fail_regs:
            out_map[key] = None
            continue
        raw = raw_by_reg.get(f["reg"])
        if raw is None:
            out_map[key] = None
            continue
        mask = (1 << f["width"]) - 1 if f["width"] < 64 else (1 << 64) - 1
        val = (raw >> f["shift"]) & mask
        out_map[key] = val
        print(f"  {f['reg']}.{f['field']} = {val:#x}", flush=True)

    print(
        f"dump: fields_parsed={sum(1 for v in out_map.values() if v is not None)}/"
        f"{len(named)} fail_regs={len(fail_regs)}",
        flush=True,
    )
    return out_map


def build_inventory(

    named: list[dict[str, Any]],
    props: set[str],
    qom_host: dict[str, int | None],
    guest_host: dict[tuple[str, str], int | None] | None = None,
    linux_cpufeature: Path | None = None,
) -> list[dict[str, Any]]:
    inv = []
    guest_host = guest_host or {}
    try:
        by_field, by_shift, _path = parse_linux_ftr_policies(linux_cpufeature)
    except SystemExit:
        by_field, by_shift = {}, {}
    for f in named:
        pr = prop_name(f["reg"], f["field"])
        writable = pr in props
        qv = qom_host.get(pr) if writable else None
        gv = guest_host.get((f["reg"], f["field"]))
        # Guest MRS for every named field; QOM only when property exists.
        hv = gv if isinstance(gv, int) else None
        pol = lookup_ftr_policy(
            by_field, by_shift, f["reg"], f["field"], f.get("shift")
        )
        ftr_policy = None
        if pol:
            ftr_policy = pol["type"]
            if pol.get("kvm_override"):
                ftr_policy = ftr_policy + " (KVM)"
        inv.append(
            {
                "register": f["reg"],
                "field": f["field"],
                "writable": writable,
                "property": pr if writable else None,
                "host": f"{hv:#x}" if isinstance(hv, int) else (None if not writable else "?"),
                "qom_host": f"{qv:#x}" if isinstance(qv, int) else None,
                "ftr_policy": ftr_policy,
                "has_arch_ranges": f["has_arch_ranges"],
                "arch_ranges": (
                    ",".join(
                        f"{a}-{b}" if a != b else str(a) for a, b in (f["ranges"] or [])
                    )
                    if f["has_arch_ranges"]
                    else ""
                ),
                "shift": f["shift"],
                "width": f["width"],
            }
        )
    return inv


def enrich_inventory_ftr_policy(
    inventory: list[dict[str, Any]],
    linux_cpufeature: Path | None = None,
) -> list[dict[str, Any]]:
    """Attach/refresh ftr_policy on an existing inventory (for --report without re-collect)."""
    try:
        by_field, by_shift, _path = parse_linux_ftr_policies(linux_cpufeature)
    except SystemExit:
        return inventory
    for e in inventory:
        pol = lookup_ftr_policy(
            by_field, by_shift, e["register"], e["field"], e.get("shift")
        )
        if not pol:
            e.setdefault("ftr_policy", None)
            continue
        e["ftr_policy"] = (
            (pol["type"] + " (KVM)") if pol.get("kvm_override") else pol["type"]
        )
    return inventory


def probe_writable_values(
    qemu: str,
    named: list[dict[str, Any]],
    props: set[str],
    host: dict[str, int | None],
    guest_host: dict[tuple[str, str], int | None] | None = None,
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    rows: list[dict[str, Any]] = []
    unexpected: list[dict[str, Any]] = []
    by_key = {(f["reg"], f["field"]): f for f in named}

    for pr in sorted(props):
        # SYSREG_<REG>_<FIELD>
        if not pr.startswith("SYSREG_"):
            continue
        body = pr[len("SYSREG_") :]
        # split reg_field: reg ends with _EL0/_EL1/_EL2 typically; use longest match
        matched = None
        for (reg, field), meta in by_key.items():
            if body == f"{reg}_{field}":
                matched = meta
                break
        if not matched:
            continue
        reg, field = matched["reg"], matched["field"]
        qom_v = host.get(pr)
        gh = (guest_host or {}).get((reg, field))
        host_v = gh if isinstance(gh, int) else qom_v
        arch, oor, arch_disp = expand_probe_values(matched)
        ok: list[str] = []
        nok: list[dict[str, str]] = []

        if arch == [] and not matched["has_arch_ranges"]:
            # wide plain sampled
            maxv = (1 << matched["width"]) - 1
            probe = []
            if isinstance(host_v, int):
                probe.append(host_v)
            for v in (0, maxv, maxv // 2):
                if v not in probe:
                    probe.append(v)
            probe_vals = probe
            oor = None
        else:
            probe_vals = list(arch)
            if oor is not None:
                probe_vals.append(oor)

        for v in probe_vals:
            is_oor = oor is not None and v == oor
            started, layer, _err = try_set(qemu, reg, field, v)
            vstr = f"{v:#x}"
            if not started:
                note = "out of arch range" if is_oor else (layer or "reject")
                nok.append({"value": vstr, "note": note})
                if note not in ("KVM", "out of arch range", "QEMU width", "no property"):
                    unexpected.append(
                        {
                            "register": reg,
                            "field": field,
                            "requested": vstr,
                            "guest": None,
                            "host": f"{host_v:#x}" if isinstance(host_v, int) else host_v,
                            "note": note,
                        }
                    )
                continue
            if isinstance(host_v, int) and host_v == v:
                ok.append(vstr)
                continue
            _exp, mrs, gerr = guest_check(qemu, reg, field, v, apply_sysreg=True)
            if mrs is None:
                note = gerr or "no guest report"
                nok.append({"value": vstr, "note": note})
                unexpected.append(
                    {
                        "register": reg,
                        "field": field,
                        "requested": vstr,
                        "guest": None,
                        "host": f"{host_v:#x}" if isinstance(host_v, int) else host_v,
                        "note": note,
                    }
                )
                continue
            if mrs == v:
                ok.append(vstr)
            else:
                note = f"guest={mrs:#x}"
                nok.append({"value": vstr, "note": note})
                unexpected.append(
                    {
                        "register": reg,
                        "field": field,
                        "requested": vstr,
                        "guest": f"{mrs:#x}",
                        "host": f"{host_v:#x}" if isinstance(host_v, int) else host_v,
                        "note": note,
                    }
                )

        rows.append(
            {
                "register": reg,
                "field": field,
                "arch_ranges": arch_disp,
                "has_arch_ranges": matched["has_arch_ranges"],
                "host": f"{host_v:#x}" if isinstance(host_v, int) else "?",
                "ok": ok,
                "nok": nok,
                "writable": True,
            }
        )
        print(
            f"probed {reg}.{field} host={host_v} ok={len(ok)} nok={len(nok)}",
            flush=True,
        )
    return rows, unexpected


def sec_name(reg: str, field: str, suffix: str) -> str:
    return f"idregs-{reg}-{field}-{suffix}".replace("_", "-")


def generate_unittests(
    inventory: list[dict[str, Any]],
    results: list[dict[str, Any]] | None = None,
    linux_cpufeature: Path | None = None,
) -> None:
    """Generate writable-field write tests from Linux ftr policy + host MRS."""
    _ = results  # must not drive pass/fail expectations
    by_field, by_shift, linux_path = parse_linux_ftr_policies(linux_cpufeature)
    print(f"Linux ftr policy: {linux_path}", flush=True)

    named = {(f["reg"], f["field"]): f for f in parse_named_fields(INC.read_text())}

    text = UNITTESTS.read_text()
    markers = [
        "# ID register SYSREG_",
        "# === ID_AA64DFR0_EL1 ===",
        "[idregs-dfr0-",
        "[idregs-ID-",
        "[idregs-pmuver-",
        "[idregs]",
    ]
    cut = -1
    for m in markers:
        i = text.find(m)
        if i >= 0 and (cut < 0 or i < cut):
            cut = i
    prefix = (text[:cut].rstrip() + "\n\n") if cut >= 0 else text.rstrip() + "\n\n"

    lines: list[str] = [
        "# ID register SYSREG_<REG>_<FIELD> - generated by idregs_* scripts",
        "# Writable fields only. Expectations from Linux cpufeature.c ftr_bits",
        "# (FTR_LOWER_SAFE / HIGHER_SAFE / EXACT / ...) vs Guest MRS host,",
        "# plus QEMU arch ranges from cpu-idregs.h.inc.",
        "# See arm/idregs-README.txt",
        "",
    ]

    n_ok = n_rej = n_skip = 0
    for inv in inventory:
        if not inv.get("writable"):
            continue
        reg, field = inv["register"], inv["field"]
        host_s = inv.get("host")
        if not host_s or host_s == "?":
            n_skip += 1
            continue
        try:
            host = int(host_s, 0)
        except ValueError:
            n_skip += 1
            continue

        meta = named.get((reg, field))
        if not meta:
            n_skip += 1
            continue

        policy = lookup_ftr_policy(
            by_field, by_shift, reg, field, meta.get("shift")
        )
        arch_vals, oor, _disp = expand_probe_values(meta)
        if arch_vals:
            candidates = list(arch_vals)
        else:
            maxv = (1 << meta["width"]) - 1
            if meta["width"] <= 4:
                candidates = list(range(maxv + 1))
            else:
                candidates = sorted({0, host, maxv, maxv // 2})
        if oor is not None and oor not in candidates:
            candidates.append(oor)
        if host not in candidates:
            candidates.append(host)

        arch_set = set(arch_vals) if meta["has_arch_ranges"] else None

        for v in sorted(set(candidates)):
            kind, reason = classify_write_expectation(
                host=host,
                value=v,
                policy=policy,
                arch_values=arch_set,
                has_arch_ranges=bool(meta["has_arch_ranges"]),
            )
            prop = f"{prop_name(reg, field)}={v:#x}"
            pol_s = policy["type"] if policy else "none"
            if kind == "ok":
                suffix = "host" if v == host else f"{v:#x}"
                lines += [
                    f"# {reg}.{field}={v:#x}: expect OK ({reason}, {pol_s}, host={host:#x})",
                    f"[{sec_name(reg, field, suffix)}]",
                    "file = idregs.flat",
                    "groups = idregs idregs-host",
                    "arch = arm64",
                    f"qemu_params = -cpu host,{prop}",
                    f"test_args = {prop}",
                    "",
                ]
                n_ok += 1
            else:
                rej_suffix = f"{v:#x}-reject"
                lines += [
                    f"# {reg}.{field}={v:#x}: expect reject ({reason}, {pol_s}, host={host:#x})",
                    f"[{sec_name(reg, field, rej_suffix)}]",
                    "file = idregs.flat",
                    "groups = idregs qemu-reject",
                    "arch = arm64",
                    f"qemu_params = -cpu host,{prop}",
                    "",
                ]
                n_rej += 1

    UNITTESTS.write_text(prefix + "\n".join(lines) + "\n")
    print(
        f"Wrote {UNITTESTS}: ok={n_ok} reject={n_rej} skipped_fields={n_skip}",
        flush=True,
    )


def esc(s: str) -> str:
    return (
        (s or "")
        .replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
    )


# Expected probe rejects — shown plain in Table 2 (not treated as test failures).
_EXPECTED_NOK_NOTES = frozenset({"KVM", "out of arch range", "QEMU policy"})


def plain_items(items: list[str]) -> str:
    if not items:
        return "—"
    return " ".join(esc(x) for x in items)


def green(items: list[str]) -> str:
    """Deprecated alias: OK values are plain (no green)."""
    return plain_items(items)


def _is_failed_nok(note: str) -> bool:
    """True for unexpected guest/result failures, not policy/arch rejects."""
    if not note:
        return True
    if note in _EXPECTED_NOK_NOTES:
        return False
    # guest=0x… / no guest report / other surprises
    return True


def red_items(nok_list: list[dict[str, str]]) -> str:
    """Format NOK values; red only for failed tests, plain for expected rejects."""
    if not nok_list:
        return "—"
    groups: OrderedDict[str, list[str]] = OrderedDict()
    for n in nok_list:
        groups.setdefault(n["note"], []).append(n["value"])
    chunks = []
    for note, vals in groups.items():
        if _is_failed_nok(note):
            body = " ".join(
                f'<font color="#b91c1c"><b>{esc(v)}</b></font>' for v in vals
            )
            chunks.append(
                f'{body} <font color="#b91c1c">({esc(note)})</font>'
            )
        else:
            body = " ".join(esc(v) for v in vals)
            chunks.append(f"{body} ({esc(note)})")
    return " · ".join(chunks)


def apply_font8(html: str) -> str:
    """Inline 8pt Arial so Google Docs HTML import keeps font size 8."""
    def wrap_cell(m: re.Match[str]) -> str:
        tag, attrs, body, end = m.group(1), m.group(2), m.group(3), m.group(4)
        if "font-size:8pt" in body or not body.strip():
            return m.group(0)
        return (
            f'<{tag}{attrs}><font face="Arial" style="font-size:8pt">'
            f"{body}</font></{end}>"
        )

    html = re.sub(
        r"<(td|th)([^>]*)>(.*?)</(td|th)>", wrap_cell, html, flags=re.S
    )

    def wrap_p(m: re.Match[str]) -> str:
        attrs, body = m.group(1), m.group(2)
        if "font-size:8pt" in body:
            return m.group(0)
        return (
            f'<p{attrs}><font face="Arial" style="font-size:8pt">'
            f"{body}</font></p>"
        )

    html = re.sub(r"<p([^>]*)>(.*?)</p>", wrap_p, html, flags=re.S)

    def wrap_h(m: re.Match[str]) -> str:
        n, attrs, body = m.group(1), m.group(2), m.group(3)
        if "font-size:8pt" in body:
            return m.group(0)
        return (
            f'<h{n}{attrs}><font face="Arial" style="font-size:8pt">'
            f"{body}</font></h{n}>"
        )

    return re.sub(r"<h([12])([^>]*)>(.*?)</h\1>", wrap_h, html, flags=re.S)



def field_bits_label(field: str, shift: Any = None, width: Any = None) -> str:
    """Format field name with bit range: FIELD[lo,hi] (lsb=shift, msb=shift+width-1)."""
    try:
        lo = int(shift)
        w = int(width)
    except (TypeError, ValueError):
        return field
    if w <= 0:
        return field
    hi = lo + w - 1
    return f"{field}[{lo},{hi}]"

def _report_html_shell(title: str, body: str) -> str:
    """Common 8pt Arial HTML wrapper for Google Docs import."""
    return f"""<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>{title}</title>
<style>
body {{ font-family: Arial, Helvetica, sans-serif; margin:1.5rem; font-size:8pt; }}
h1 {{ font-size:8pt; font-weight:bold; }} h2 {{ font-size:8pt; font-weight:bold; margin-top:1.2rem; }}
.meta {{ color:#57534e; font-size:8pt; }}
table {{ border-collapse:collapse; width:100%; font-size:8pt; margin-bottom:1rem; }}
th, td {{ border:1px solid #d6d3d1; padding:2pt 4pt; text-align:left; vertical-align:top; font-size:8pt; }}
th {{ background:#f5f5f4; font-size:8pt; }}
p, td, th, li {{ font-size:8pt; }}
</style></head><body style="font-size:8pt;font-family:Arial,Helvetica,sans-serif">
<font face="Arial" size="1" style="font-size:8pt">
{body}
</font>
</body></html>
"""


def write_inventory_report(data: dict[str, Any], html_path: Path) -> None:
    """Table 1 — named fields: writability, safe policy, QOM, Guest MRS."""
    inv = enrich_inventory_ftr_policy(list(data.get("inventory") or []))
    data["inventory"] = inv
    inv_rows = []
    for e in inv:
        field_cell = esc(
            field_bits_label(e["field"], e.get("shift"), e.get("width"))
        )
        if e["writable"]:
            field_cell = f"<b>{field_cell}</b>"
        qom = e.get("qom_host") or ("—" if not e["writable"] else "?")
        mrs = e["host"] or "—"
        policy = e.get("ftr_policy") or "—"
        diverge = (
            e["writable"]
            and isinstance(e.get("qom_host"), str)
            and isinstance(e.get("host"), str)
            and e["qom_host"] != e["host"]
        )
        tr = '<tr bgcolor="#fecaca">' if diverge else "<tr>"
        inv_rows.append(
            "{}<td>{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td></tr>".format(
                tr,
                esc(e["register"]),
                field_cell,
                "yes" if e["writable"] else "no",
                esc(policy),
                esc(qom),
                esc(mrs),
                esc(e.get("arch_ranges") or ""),
            )
        )
    stamp = data.get("generated") or ""
    body = f"""<h1 style="font-size:8pt">SYSREG ID-field inventory (Table 1)</h1>
<p class="meta">{esc(host_type_name(data.get("host_name")))} · {esc(stamp)} ·
named fields={len(inv)} · writable={sum(1 for e in inv if e["writable"])}</p>

<h2 style="font-size:8pt">Table 1 — All named ID-reg fields (writability + host defaults)</h2>
<p class="meta"><b>Safe policy</b> = Linux <code>arm64_ftr_bits</code> type from
<code>arch/arm64/kernel/cpufeature.c</code> (e.g. FTR_LOWER_SAFE); "(KVM)" marks a
KVM override of the host table. <b>QOM host</b> = SYSREG property via QOM with plain
<code>-cpu host</code>. <b>Guest MRS</b> = field value in the guest with the same config.
Field column shows <code>NAME[lo,hi]</code> bit positions. Writable names are <b>bold</b>. Rows where QOM host ≠ Guest MRS are shaded red.
Arch ranges filled only when the header defines IDREG_ARCH_VALUE_RANGE.</p>
<table style="font-size:8pt;font-family:Arial,Helvetica,sans-serif" border="1" cellpadding="2" cellspacing="0">
<thead><tr>
<th>Register</th><th>Field</th><th>Writable</th><th>Safe policy</th><th>QOM host</th><th>Guest MRS</th><th>Arch ranges</th>
</tr></thead>
<tbody>
{''.join(inv_rows)}
</tbody></table>
"""
    html = apply_font8(_report_html_shell("SYSREG ID-reg inventory", body))
    html_path.parent.mkdir(parents=True, exist_ok=True)
    html_path.write_text(html, encoding="utf-8")
    print(f"Wrote {html_path}", flush=True)


def parse_idregs_unittest_name(
    name: str, field_keys: list[tuple[str, str]]
) -> tuple[str, str, str, bool] | None:
    """Map idregs-<reg>-<field>-<suffix> → (reg, field, suffix, is_reject)."""
    if not name.startswith("idregs-"):
        return None
    rest = name[len("idregs-") :]
    is_reject = rest.endswith("-reject")
    if is_reject:
        rest = rest[: -len("-reject")]
    # Longest match: sec_name replaces '_' → '-' on the whole string
    best: tuple[str, str, str] | None = None
    best_len = -1
    for reg, field in field_keys:
        prefix = f"{reg}-{field}-".replace("_", "-")
        if rest.startswith(prefix) and len(prefix) > best_len:
            best = (reg, field, rest[len(prefix) :])
            best_len = len(prefix)
    if not best:
        return None
    reg, field, suffix = best
    return reg, field, suffix, is_reject


_ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
_RESULT_LINE = re.compile(
    r"^(PASS|FAIL|SKIP)\s+(\S+)(?:\s+\((.*)\))?\s*$"
)


def _strip_ansi(s: str) -> str:
    return _ANSI_RE.sub("", s)


def _accumulate_run_events(
    events: list[tuple[str, str, str]],
    inventory: list[dict[str, Any]],
) -> tuple[list[dict[str, Any]], list[dict[str, Any]], dict[str, int]]:
    """events: list of (status, testname, reason)."""
    named = {(f["reg"], f["field"]): f for f in parse_named_fields(INC.read_text())}
    inv_by = {(e["register"], e["field"]): e for e in inventory}
    keys = sorted(
        {(e["register"], e["field"]) for e in inventory if e.get("writable")},
        key=lambda k: len(f"{k[0]}-{k[1]}".replace("_", "-")),
        reverse=True,
    )

    acc: dict[tuple[str, str], dict[str, Any]] = {}
    unexpected: list[dict[str, str]] = []
    counts = {"pass": 0, "fail": 0, "skip": 0, "other": 0}

    def ensure(reg: str, field: str) -> dict[str, Any]:
        key = (reg, field)
        if key not in acc:
            inv = inv_by.get(key) or {}
            meta = named.get(key)
            arch_disp = ""
            has_arch = False
            if meta:
                _arch, _oor, arch_disp = expand_probe_values(meta)
                has_arch = bool(meta.get("has_arch_ranges"))
            host_s = inv.get("host") or "?"
            acc[key] = {
                "register": reg,
                "field": field,
                "arch_ranges": arch_disp,
                "has_arch_ranges": has_arch,
                "host": host_s,
                "ok": [],
                "nok": [],
                "writable": True,
            }
        return acc[key]

    for status, tname, reason in events:
        status_l = status.lower()
        if status_l in counts:
            counts[status_l] += 1
        else:
            counts["other"] += 1

        parsed = parse_idregs_unittest_name(tname, keys)
        if not parsed:
            continue
        reg, field, suffix, is_reject = parsed
        row = ensure(reg, field)
        try:
            host_v = int(row["host"], 0) if row["host"] not in ("?", "") else None
        except ValueError:
            host_v = None

        if suffix == "host":
            vstr = f"{host_v:#x}" if host_v is not None else "host"
        else:
            try:
                vstr = f"{int(suffix, 0):#x}"
            except ValueError:
                vstr = suffix

        if is_reject:
            if status == "PASS":
                note = "QEMU policy"
                low = reason.lower()
                if "kvm" in low:
                    note = "KVM"
                elif "arch" in low or "range" in low or "invalid argument" in low:
                    note = "out of arch range" if "range" in low or "arch" in low else "QEMU policy"
                if "invalid argument" in low or "fail to apply" in low:
                    note = "QEMU policy"
                row["nok"].append({"value": vstr, "note": note})
            elif status == "FAIL":
                note = reason or "qemu accepted config; expected reject"
                row["nok"].append({"value": vstr, "note": note})
                unexpected.append(
                    {
                        "register": reg,
                        "field": field,
                        "requested": vstr,
                        "guest": None,
                        "host": row["host"],
                        "note": note,
                    }
                )
            continue

        if status == "PASS":
            if vstr not in row["ok"]:
                row["ok"].append(vstr)
        elif status == "FAIL":
            note = reason or "test failed"
            row["nok"].append({"value": vstr, "note": note})
            unexpected.append(
                {
                    "register": reg,
                    "field": field,
                    "requested": vstr,
                    "guest": None,
                    "host": row["host"],
                    "note": note,
                }
            )

    rows = []
    for key in sorted(acc.keys(), key=lambda k: (k[0], k[1])):
        r = acc[key]
        r["ok"] = sorted(
            r["ok"], key=lambda s: int(s, 0) if str(s).startswith("0x") else s
        )
        rows.append(r)
    return rows, unexpected, counts


def parse_run_stdout(
    text: str, inventory: list[dict[str, Any]]
) -> tuple[list[dict[str, Any]], list[dict[str, Any]], dict[str, int]]:
    """Build Table 2 rows from run_tests.sh PASS/FAIL lines (ANSI-safe)."""
    events: list[tuple[str, str, str]] = []
    for line in text.splitlines():
        m = _RESULT_LINE.match(_strip_ansi(line).strip())
        if not m:
            continue
        events.append((m.group(1), m.group(2), m.group(3) or ""))
    return _accumulate_run_events(events, inventory)


def parse_run_logs(
    log_dir: Path, inventory: list[dict[str, Any]]
) -> tuple[list[dict[str, Any]], list[dict[str, Any]], dict[str, int]]:
    """Rebuild Table 2 from logs/*.log after a completed --run."""
    events: list[tuple[str, str, str]] = []
    for path in sorted(log_dir.glob("idregs-*.log")):
        tname = path.stem
        text = path.read_text(errors="replace")
        is_reject = tname.endswith("-reject")
        if is_reject:
            if re.search(
                r"fail to apply|Invalid argument|error|can't apply|cannot",
                text,
                re.I,
            ):
                events.append(("PASS", tname, text.strip().splitlines()[-1] if text.strip() else ""))
            elif "PASS: idregs:" in text or "SUMMARY:" in text:
                events.append(("FAIL", tname, "qemu accepted config; expected reject"))
            else:
                events.append(("PASS", tname, text.strip().splitlines()[-1] if text.strip() else "reject"))
        else:
            if re.search(r"^PASS: idregs:", text, re.M):
                events.append(("PASS", tname, ""))
            elif re.search(r"^FAIL:|^ERROR:", text, re.M):
                # last non-empty line as reason
                reason = ""
                for line in reversed(text.splitlines()):
                    if line.strip():
                        reason = line.strip()
                        break
                events.append(("FAIL", tname, reason))
            else:
                events.append(("FAIL", tname, "no PASS in log"))
    return _accumulate_run_events(events, inventory)

def run_kut_group(
    qemu: str, group: str, parallel: int, capture: bool = False
) -> tuple[int, str]:
    cmd = [str(REPO / "run_tests.sh"), "-g", group]
    if parallel > 1:
        cmd += ["-j", str(parallel)]
    print("Running:", " ".join(cmd), flush=True)
    env = {**os.environ, "QEMU": qemu}
    if not capture:
        p = subprocess.run(cmd, cwd=str(REPO), env=env)
        return p.returncode, ""
    # Stream lines live and keep a copy for Table 2 parsing
    p = subprocess.Popen(
        cmd,
        cwd=str(REPO),
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    chunks: list[str] = []
    assert p.stdout is not None
    for line in p.stdout:
        chunks.append(line)
        sys.stdout.write(line)
        sys.stdout.flush()
    rc = p.wait()
    return rc, "".join(chunks)


def write_probe_report(data: dict[str, Any], html_path: Path) -> None:
    """Table 2 — writable-field OK/NOK from --run (or legacy --probe)."""
    results = data.get("results") or []
    unexpected = data.get("unexpected") or []
    source = data.get("results_source") or (
        "run" if data.get("run_counts") else ("probe" if results else "")
    )
    inv = enrich_inventory_ftr_policy(list(data.get("inventory") or []))
    data["inventory"] = inv
    meta_by_field = {
        (e["register"], e["field"]): e for e in inv
    }
    named = {(f["reg"], f["field"]): f for f in parse_named_fields(INC.read_text())}
    try:
        by_field, by_shift, _linux = parse_linux_ftr_policies()
    except SystemExit:
        by_field, by_shift = {}, {}

    def field_cell(reg: str, field: str, bold: bool = False) -> str:
        meta = meta_by_field.get((reg, field)) or {}
        name = esc(
            field_bits_label(field, meta.get("shift"), meta.get("width"))
        )
        if bold:
            name = f"<b>{name}</b>"
        pol = meta.get("ftr_policy")
        if pol:
            return (
                f'{name}<br><font color="#57534e" style="font-size:8pt">'
                f"{esc(pol)}</font>"
            )
        return name

    def ok_vs_policy_mismatch(row: dict[str, Any]) -> bool:
        """True when probe OK disagrees with ftr expectation, or FTR_EXACT
        accepted a non-host value (e.g. CTR_EL0.L1Ip PIPT→VIPT)."""
        reg, field = row["register"], row["field"]
        meta = named.get((reg, field)) or meta_by_field.get((reg, field))
        if not meta:
            return False
        try:
            host = int(row.get("host") or meta_by_field.get((reg, field), {}).get("host") or "", 0)
        except (TypeError, ValueError):
            return False
        ok_vals: set[int] = set()
        for s in row.get("ok") or []:
            try:
                ok_vals.add(int(s, 0))
            except ValueError:
                pass
        policy = lookup_ftr_policy(
            by_field, by_shift, reg, field, meta.get("shift")
        )
        arch_vals, oor, _ = expand_probe_values(
            {
                "width": meta.get("width"),
                "has_arch_ranges": meta.get("has_arch_ranges"),
                "ranges": meta.get("ranges")
                or (
                    named.get((reg, field)) or {}
                ).get("ranges"),
            }
        )
        # named fields use "ranges"; inventory may not — prefer named
        nmeta = named.get((reg, field))
        if nmeta:
            arch_vals, oor, _ = expand_probe_values(nmeta)
            has_arch = bool(nmeta.get("has_arch_ranges"))
            width = nmeta["width"]
        else:
            has_arch = bool(meta.get("has_arch_ranges"))
            width = int(meta.get("width") or 4)
        if arch_vals:
            candidates = list(arch_vals)
        else:
            maxv = (1 << width) - 1
            candidates = (
                list(range(maxv + 1)) if width <= 4 else sorted({0, host, maxv, maxv // 2})
            )
        if oor is not None:
            candidates.append(oor)
        candidates.append(host)
        arch_set = set(arch_vals) if has_arch else None
        expected_ok: set[int] = set()
        for v in sorted(set(candidates)):
            kind, _reason = classify_write_expectation(
                host=host,
                value=v,
                policy=policy,
                arch_values=arch_set,
                has_arch_ranges=has_arch,
            )
            if kind == "ok":
                expected_ok.add(v)
        if ok_vals != expected_ok:
            return True
        # FTR_EXACT but probe accepted something other than host (safe_val downgrade etc.)
        if policy and policy.get("type") == "FTR_EXACT" and (ok_vals - {host}):
            return True
        return False

    def format_ok(row: dict[str, Any]) -> str:
        """Plain OK values; bold those that are non-host under FTR_EXACT."""
        items = row.get("ok") or []
        if not items:
            return "—"
        try:
            host = int(row.get("host") or "", 0)
        except ValueError:
            host = None
        meta = meta_by_field.get((row["register"], row["field"])) or {}
        exact = (meta.get("ftr_policy") or "").startswith("FTR_EXACT")
        parts = []
        for s in items:
            try:
                v = int(s, 0)
            except ValueError:
                parts.append(esc(s))
                continue
            if exact and host is not None and v != host:
                parts.append(f"<b>{esc(s)}</b>")
            else:
                parts.append(esc(s))
        return " ".join(parts)

    res_rows = []
    for f in results:
        arch = esc(f["arch_ranges"]) if f.get("has_arch_ranges") else ""
        mismatch = ok_vs_policy_mismatch(f)
        tr = '<tr bgcolor="#fef08a">' if mismatch else "<tr>"
        res_rows.append(
            "{}<td>{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td></tr>".format(
                tr,
                esc(f["register"]),
                field_cell(f["register"], f["field"], bold=True),
                arch,
                esc(f["host"]),
                format_ok(f),
                red_items(f["nok"]),
            )
        )

    unexp_rows = []
    for u in unexpected:
        ureg, ufield = u.get("register") or "", u.get("field") or ""
        unexp_rows.append(
            "<tr><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td></tr>".format(
                esc(ureg),
                field_cell(ureg, ufield, bold=False) if ufield else "—",
                esc(str(u.get("host") or "—")),
                esc(str(u.get("requested") or "")),
                esc(str(u.get("note") or "")),
            )
        )

    stamp = data.get("generated") or ""
    run_counts = data.get("run_counts") or {}
    if source == "run" and run_counts:
        stats = (
            f"fields={len(results)} · unexpected={len(unexpected)} · "
            f"PASS={run_counts.get('pass', 0)} FAIL={run_counts.get('fail', 0)} "
            f"SKIP={run_counts.get('skip', 0)}"
        )
        empty_msg = "(no run results — run --run)"
        title = "SYSREG ID-field write tests (Table 2)"
        ok_help = (
            "OK = idregs-host test PASS (value accepted + guest MRS matches). "
            "Expected rejects (qemu-reject PASS) are plain in NOK. "
        )
        yellow_help = (
            "Rows shaded <font bgcolor=\"#fef08a\">yellow</font> when test OK set does not match "
            "the Linux ftr expectation, or when <b>FTR_EXACT</b> accepted a non-host value "
            "(e.g. CTR_EL0.L1Ip PIPT host allowing VIPT). Non-host OK values in that case are bold."
        )
    else:
        stats = (
            f"writable probed={len(results)} · unexpected={len(unexpected)}"
        )
        empty_msg = "(no results — run --run, or legacy --probe)"
        title = "SYSREG ID-field write tests (Table 2)"
        ok_help = (
            "OK = accepted + guest MRS matches (plain). "
            "Expected rejects (KVM / arch / QEMU policy) are plain in NOK. "
        )
        yellow_help = (
            "Rows shaded <font bgcolor=\"#fef08a\">yellow</font> when OK does not match "
            "the Linux ftr expectation, or when <b>FTR_EXACT</b> accepted a non-host value "
            "(e.g. CTR_EL0.L1Ip PIPT host allowing VIPT). Non-host OK values in that case are bold."
        )

    body = f"""<h1 style="font-size:8pt">{title}</h1>
<p class="meta">{esc(host_type_name(data.get("host_name")))} · {esc(stamp)} ·
{stats} · inventory fields={len(inv)} · source={esc(source or "?")}</p>

<h2 style="font-size:8pt">Unexpected failures</h2>
<table style="font-size:8pt;font-family:Arial,Helvetica,sans-serif" border="1" cellpadding="2" cellspacing="0">
<thead><tr><th>Register</th><th>Field</th><th>Host</th><th>Value</th><th>Detail</th></tr></thead>
<tbody>
{''.join(unexp_rows) or '<tr><td colspan="5">(none)</td></tr>'}
</tbody></table>

<h2 style="font-size:8pt">Table 2 — Writable fields: OK / NOK</h2>
<p class="meta">{ok_help}
<font color="#b91c1c"><b>Red</b></font> = failed test (unexpected result).
{yellow_help}
Field column shows <code>NAME[lo,hi]</code>; safe policy under the name.</p>
<table style="font-size:8pt;font-family:Arial,Helvetica,sans-serif" border="1" cellpadding="2" cellspacing="0">
<thead><tr>
<th>Register</th><th>Field</th><th>Arch ranges</th><th>Guest MRS</th>
<th>OK</th><th>NOK / unexpected</th>
</tr></thead>
<tbody>
{''.join(res_rows) or f'<tr><td colspan="6">{empty_msg}</td></tr>'}
</tbody></table>
"""
    html = apply_font8(_report_html_shell("SYSREG ID-reg write tests", body))
    html_path.parent.mkdir(parents=True, exist_ok=True)
    html_path.write_text(html, encoding="utf-8")
    print(f"Wrote {html_path}", flush=True)


def write_report(data: dict[str, Any], html_path: Path | None = None) -> None:
    """Write both reports (legacy helper). Prefer the per-script writers."""
    write_inventory_report(data, DEFAULT_INVENTORY_HTML)
    write_probe_report(data, DEFAULT_PROBE_HTML)
    if html_path is not None and html_path.resolve() not in {
        DEFAULT_INVENTORY_HTML.resolve(),
        DEFAULT_PROBE_HTML.resolve(),
    }:
        # Combined document at the requested path
        inv_text = DEFAULT_INVENTORY_HTML.read_text(encoding="utf-8")
        probe_text = DEFAULT_PROBE_HTML.read_text(encoding="utf-8")
        inv_m = re.search(
            r'<font face="Arial" size="1" style="font-size:8pt">\s*(.*?)\s*</font>\s*</body>',
            inv_text,
            flags=re.S,
        )
        probe_m = re.search(
            r'<font face="Arial" size="1" style="font-size:8pt">\s*(.*?)\s*</font>\s*</body>',
            probe_text,
            flags=re.S,
        )
        if inv_m and probe_m:
            body = inv_m.group(1) + "\n" + probe_m.group(1)
            html = apply_font8(_report_html_shell("SYSREG ID-reg report", body))
            html_path.parent.mkdir(parents=True, exist_ok=True)
            html_path.write_text(html, encoding="utf-8")
            print(f"Wrote {html_path}", flush=True)


def shutil_which(cmd: str) -> str | None:
    from shutil import which

    return which(cmd)


