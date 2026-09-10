ID-register SYSREG tests
========================

Needs: aarch64+KVM, QEMU with SYSREG_* CPU properties, LINUX_SRC (kernel
tree with arch/arm64/kernel/cpufeature.c), this tree configured for arm64.

  export QEMU=/path/to/qemu-system-aarch64
  export LINUX_SRC=/path/to/linux

Mandatory steps
---------------

  # 1) Inventory: writability, QOM host, Guest MRS (one dump boot)
  python3 scripts/idregs_inventory.py --collect --qemu "$QEMU"

  # 2) Table 1 HTML → scripts/idregs/idregs-sysreg-inventory.html
  python3 scripts/idregs_inventory.py --report

  # 3) Generate writable-field write / reject tests (Linux ftr policy)
  python3 scripts/idregs_write_probe.py --gen-unittests

  # 4) Run tests and write Table 2 HTML from PASS/FAIL
  #    → scripts/idregs/idregs-sysreg-probe-report.html
  python3 scripts/idregs_write_probe.py --run -j 8

Or: python3 scripts/idregs_write_probe.py --all -j 8
     (= --gen-unittests --run, includes Table 2 report)

JSON: scripts/idregs/idregs-sysreg-probe.json

Groups: idregs / idregs-host (expect OK), qemu-reject (expect refuse).

Optional
--------

  # Re-render Table 2 from existing JSON (after a prior --run)
  python3 scripts/idregs_write_probe.py --report

  # Legacy empirical probe (not used for test expectations)
  python3 scripts/idregs_write_probe.py --probe
