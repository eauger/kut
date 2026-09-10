#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Deprecated — use idregs_inventory.py and idregs_write_probe.py."""
import sys
msg = (
    "Split into:"
    + chr(10) + "  python3 scripts/idregs_inventory.py --help"
    + chr(10) + "  python3 scripts/idregs_write_probe.py --help"
    + chr(10) + "See arm/idregs-README.txt."
)
print(msg, file=sys.stderr)
raise SystemExit(2)
