#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Parse Linux arm64 ftr_bits policies and classify SYSREG write expectations."""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path
from typing import Any


def die(msg: str, code: int = 1) -> None:
    print(msg, file=sys.stderr)
    raise SystemExit(code)


def norm_field(name: str) -> str:
    return re.sub(r"[^a-z0-9]", "", (name or "").lower())


def default_linux_cpufeature() -> Path:
    env = os.environ.get("LINUX_SRC") or os.environ.get("LINUX")
    candidates: list[Path] = []
    if env:
        p = Path(env)
        candidates += [p, p / "arch/arm64/kernel/cpufeature.c"]
    home = Path.home()
    candidates += [
        home / "UPSTREAM/linux/arch/arm64/kernel/cpufeature.c",
        Path("/home/augere/UPSTREAM/linux/arch/arm64/kernel/cpufeature.c"),
    ]
    for c in candidates:
        if c.is_file():
            return c
        if c.is_dir():
            p = c / "arch/arm64/kernel/cpufeature.c"
            if p.is_file():
                return p
    die(
        "linux cpufeature.c not found; set LINUX_SRC to the kernel tree "
        "(expects arch/arm64/kernel/cpufeature.c)"
    )
    raise AssertionError



def resolve_safe_val(safe_tok: str, cpufeature_text: str) -> int | None:
    """Resolve ftr safe_val token to int (numeric or CTR_EL0_L1Ip_VIPT-style define)."""
    tok = (safe_tok or "").strip()
    if not tok:
        return None
    try:
        return int(tok, 0)
    except ValueError:
        pass
    # Common pattern: enum/macro name ending in _VIPT / _PIPT / etc.
    # Prefer sysreg.h-style: #define NAME value  or  NAME = value in enum
    m = re.search(
        rf"(?:#define\s+{re.escape(tok)}\s+|\b{re.escape(tok)}\s*=\s*)"
        r"(0x[0-9a-fA-F]+|\d+)\b",
        cpufeature_text,
    )
    if m:
        return int(m.group(1), 0)
    # Known CTR_EL0 L1Ip encodings (ARM DDI)
    known = {
        "CTR_EL0_L1Ip_RESERVED_VPIPT": 0,
        "CTR_EL0_L1Ip_RESERVED_AIVIVT": 1,
        "CTR_EL0_L1Ip_VIPT": 2,
        "CTR_EL0_L1Ip_PIPT": 3,
    }
    if tok in known:
        return known[tok]
    # Search linux headers next to cpufeature if present
    return None

def parse_linux_ftr_policies(
    cpufeature_c: Path | None = None,
) -> tuple[dict[tuple[str, str], dict[str, Any]], dict[tuple[str, int], dict[str, Any]], Path]:
    path = cpufeature_c or default_linux_cpufeature()
    text = path.read_text()

    table_to_regs: dict[str, list[str]] = {}
    for m in re.finditer(
        r"ARM64_FTR_REG(?:_OVERRIDE)?\(\s*(SYS_[A-Z0-9_]+)\s*,\s*(ftr_[a-z0-9_]+)",
        text,
    ):
        sys_id, table = m.group(1), m.group(2)
        reg = sys_id[4:] if sys_id.startswith("SYS_") else sys_id
        table_to_regs.setdefault(table, []).append(reg)
    table_to_regs.setdefault("ftr_ctr", []).append("CTR_EL0")

    arrays: dict[str, str] = {}
    arr_pat = (
        r"static const struct arm64_ftr_bits (ftr_[a-z0-9_]+)\[\] = "
        r"\{([\s\S]*?)\n\};"
    )
    for m in re.finditer(arr_pat, text):
        arrays[m.group(1)] = m.group(2)

    bit_re = re.compile(
        r"(S_)?ARM64_FTR_BITS\(\s*FTR_(?:VISIBLE|HIDDEN)"
        r"(?:_IF_IS_ENABLED\([^)]*\))?\s*,\s*"
        r"FTR_(?:STRICT|NONSTRICT)\s*,\s*"
        r"(FTR_[A-Z_]+)\s*,\s*"
        r"([^,]+)\s*,\s*"
        r"([^,]+)\s*,\s*"
        r"([^)]+)\)"
    )

    by_field: dict[tuple[str, str], dict[str, Any]] = {}
    by_shift: dict[tuple[str, int], dict[str, Any]] = {}

    for table, body in arrays.items():
        regs = table_to_regs.get(table) or []
        for bm in bit_re.finditer(body):
            signed = bool(bm.group(1))
            ftype = bm.group(2)
            shift_tok = bm.group(3).strip()
            width_tok = bm.group(4).strip()
            safe_tok = bm.group(5).strip()
            try:
                width = int(width_tok, 0)
            except ValueError:
                continue
            safe_val = resolve_safe_val(safe_tok, text)

            field: str | None = None
            shift: int | None = None
            if re.fullmatch(r"\d+", shift_tok):
                shift = int(shift_tok)
            elif shift_tok.endswith("_SHIFT"):
                name = shift_tok[: -len("_SHIFT")]
                for reg in regs:
                    if name.startswith(reg + "_"):
                        field = name[len(reg) + 1 :]
                        break
            else:
                continue

            for reg in regs:
                pol = {
                    "type": ftype,
                    "safe_val": safe_val,
                    "signed": signed,
                    "width": width,
                    "table": table,
                    "linux_field": field,
                    "shift": shift,
                    "reg": reg,
                }
                if reg == "ID_AA64DFR0_EL1" and field in ("PMUVer", "DebugVer"):
                    pol["type"] = "FTR_LOWER_SAFE"
                    pol["kvm_override"] = True
                if reg == "ID_DFR0_EL1" and field == "PerfMon":
                    pol["type"] = "FTR_LOWER_SAFE"
                    pol["kvm_override"] = True
                if field is not None:
                    by_field[(reg, norm_field(field))] = pol
                if shift is not None:
                    by_shift[(reg, shift)] = pol

    return by_field, by_shift, path


def lookup_ftr_policy(
    by_field: dict[tuple[str, str], dict[str, Any]],
    by_shift: dict[tuple[str, int], dict[str, Any]],
    reg: str,
    field: str,
    shift: int | None = None,
) -> dict[str, Any] | None:
    p = by_field.get((reg, norm_field(field)))
    if p:
        return p
    if shift is not None:
        return by_shift.get((reg, shift))
    return None


def classify_write_expectation(
    *,
    host: int | None,
    value: int,
    policy: dict[str, Any] | None,
    arch_values: set[int] | None,
    has_arch_ranges: bool,
) -> tuple[str, str]:
    """Return (kind, reason) with kind in {"ok", "reject"}."""
    if has_arch_ranges and arch_values is not None and value not in arch_values:
        return "reject", "qemu-arch-range"
    if host is None:
        return "reject", "no-host-mrs"
    if value == host:
        return "ok", "host-equal"
    if policy is None:
        return "reject", "no-ftr-policy"

    width = int(policy.get("width") or 4)
    signed = bool(policy.get("signed"))
    ptype = policy["type"]
    exact = policy.get("safe_val")

    def as_signed(v: int) -> int:
        sign_bit = 1 << (width - 1)
        mask = (1 << width) - 1
        v &= mask
        return v - (1 << width) if (v & sign_bit) else v

    if ptype == "FTR_LOWER_SAFE":
        ok = (as_signed(value) <= as_signed(host)) if signed else (value <= host)
        return ("ok" if ok else "reject", "FTR_LOWER_SAFE")
    if ptype == "FTR_HIGHER_SAFE":
        ok = (as_signed(value) >= as_signed(host)) if signed else (value >= host)
        return ("ok" if ok else "reject", "FTR_HIGHER_SAFE")
    if ptype == "FTR_HIGHER_OR_ZERO_SAFE":
        if host == 0 or value == 0:
            ok = value == 0
        else:
            ok = value >= host
        return ("ok" if ok else "reject", "FTR_HIGHER_OR_ZERO_SAFE")
    if ptype == "FTR_EXACT":
        if exact is None:
            return "reject", "FTR_EXACT"
        return ("ok" if value == exact else "reject", "FTR_EXACT")
    return "reject", "unknown-policy"
