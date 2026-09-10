/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Verify SYSREG_<REG>_<FIELD>=<val> (test_args / -append) against guest MRS.
 * Pair with QEMU -cpu ...,SYSREG_<REG>_<FIELD>=<val>.
 *
 * Also supports test_args=dump: one boot that prints Guest MRS for every
 * named ID-reg field (used by scripts/idregs_inventory.py --collect).
 */

#include <libcflat.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>
#include <asm/sysreg.h>
#include <asm/processor.h>
#include "cpu-sysregs-enc.h"

typedef struct {
	const char *reg_name;
	const char *field_name;
	uint8_t shift;
	uint8_t width;
} sysreg_field_map_t;

#define IDREG_START(reg)
#define IDREG_END(reg)
#define IDREG_FIELD(reg, field, shift, len) \
	{ #reg, #field, shift, len },
#define IDREG_ARCH_VALUE_RANGE(_lo, _hi)
#define IDREG_FIELD_ENUM(reg, field, shift, len, ...) \
	{ #reg, #field, shift, len },

static const sysreg_field_map_t sysreg_table[] = {
#include "cpu-idregs.h.inc"
	{ NULL, NULL, 0, 0 }
};

#undef IDREG_START
#undef IDREG_END
#undef IDREG_FIELD
#undef IDREG_ARCH_VALUE_RANGE
#undef IDREG_FIELD_ENUM

static bool read_sysreg_by_name(const char *reg_name, uint64_t *val)
{
#define TRY(reg) \
	if (strcasecmp(reg_name, #reg) == 0) { \
		*val = read_sysreg_s(SYSENC_##reg); \
		return true; \
	}
	TRY(AIDR_EL1);
	TRY(CCSIDR_EL1);
	TRY(CCSIDR2_EL1);
	TRY(CLIDR_EL1);
	TRY(CTR_EL0);
	TRY(DCZID_EL0);
	TRY(GMID_EL1);
	TRY(ID_AA64AFR0_EL1);
	TRY(ID_AA64AFR1_EL1);
	TRY(ID_AA64DFR0_EL1);
	TRY(ID_AA64DFR1_EL1);
	TRY(ID_AA64DFR2_EL1);
	TRY(ID_AA64FPFR0_EL1);
	TRY(ID_AA64ISAR0_EL1);
	TRY(ID_AA64ISAR1_EL1);
	TRY(ID_AA64ISAR2_EL1);
	TRY(ID_AA64ISAR3_EL1);
	TRY(ID_AA64MMFR0_EL1);
	TRY(ID_AA64MMFR1_EL1);
	TRY(ID_AA64MMFR2_EL1);
	TRY(ID_AA64MMFR3_EL1);
	TRY(ID_AA64MMFR4_EL1);
	TRY(ID_AA64PFR0_EL1);
	TRY(ID_AA64PFR1_EL1);
	TRY(ID_AA64PFR2_EL1);
	TRY(ID_AA64SMFR0_EL1);
	TRY(ID_AA64ZFR0_EL1);
	TRY(ID_AFR0_EL1);
	TRY(ID_DFR0_EL1);
	TRY(ID_DFR1_EL1);
	TRY(ID_ISAR0_EL1);
	TRY(ID_ISAR1_EL1);
	TRY(ID_ISAR2_EL1);
	TRY(ID_ISAR3_EL1);
	TRY(ID_ISAR4_EL1);
	TRY(ID_ISAR5_EL1);
	TRY(ID_ISAR6_EL1);
	TRY(ID_MMFR0_EL1);
	TRY(ID_MMFR1_EL1);
	TRY(ID_MMFR2_EL1);
	TRY(ID_MMFR3_EL1);
	TRY(ID_MMFR4_EL1);
	TRY(ID_MMFR5_EL1);
	TRY(ID_PFR0_EL1);
	TRY(ID_PFR1_EL1);
	TRY(ID_PFR2_EL1);
	TRY(MIDR_EL1);
	TRY(MPIDR_EL1);
	TRY(MVFR0_EL1);
	TRY(MVFR1_EL1);
	TRY(MVFR2_EL1);
	TRY(REVIDR_EL1);
	TRY(SMIDR_EL1);
#undef TRY
	return false;
}


static volatile int mrs_faulted;

static void sysreg_fault_handler(struct pt_regs *regs, unsigned int esr)
{
	mrs_faulted = 1;
	regs->pc += 4;
}


/*
 * Machine-parseable dump for inventory collect (one QEMU boot):
 *   IDREGS_DUMP_BEGIN
 *   IDREGS_DUMP REG=<name> RAW=0x...
 *   IDREGS_DUMP REG=<name> FAIL=<reason>
 *   IDREGS_DUMP FIELD=<reg>.<field> VAL=0x...
 *   IDREGS_DUMP_END
 */
static void idregs_dump_all(void)
{
	const char *prev_reg = NULL;
	int n_ok = 0, n_fail = 0, n_fields = 0;
	uint64_t reg_val = 0;
	bool have_raw = false;

	/* ESR_ELx_EC_SYS64=0x18, ESR_ELx_EC_UNKNOWN=0x00 */
	install_exception_handler(EL1H_SYNC, 0x18, sysreg_fault_handler);
	install_exception_handler(EL1H_SYNC, 0x00, sysreg_fault_handler);

	printf("IDREGS_DUMP_BEGIN\n");

	for (const sysreg_field_map_t *m = sysreg_table; m->reg_name; m++) {
		uint64_t mask, field_val;

		if (!prev_reg || strcasecmp(prev_reg, m->reg_name) != 0) {
			prev_reg = m->reg_name;
			mrs_faulted = 0;
			if (!read_sysreg_by_name(m->reg_name, &reg_val)) {
				have_raw = false;
				printf("IDREGS_DUMP REG=%s FAIL=mrs-not-implemented\n",
				       m->reg_name);
				n_fail++;
			} else if (mrs_faulted) {
				have_raw = false;
				printf("IDREGS_DUMP REG=%s FAIL=guest-abort\n",
				       m->reg_name);
				n_fail++;
			} else {
				have_raw = true;
				printf("IDREGS_DUMP REG=%s RAW=0x%" PRIx64 "\n",
				       m->reg_name, reg_val);
				n_ok++;
			}
		}

		if (!have_raw)
			continue;

		mask = (m->width >= 64) ? ~0ULL : ((1ULL << m->width) - 1);
		field_val = (reg_val >> m->shift) & mask;
		printf("IDREGS_DUMP FIELD=%s.%s VAL=0x%" PRIx64 "\n",
		       m->reg_name, m->field_name, field_val);
		n_fields++;
	}

	printf("IDREGS_DUMP_END ok_regs=%d fail_regs=%d fields=%d\n",
	       n_ok, n_fail, n_fields);

	install_exception_handler(EL1H_SYNC, 0x18, NULL);
	install_exception_handler(EL1H_SYNC, 0x00, NULL);

	report(n_ok > 0, "dump complete (%d regs ok, %d fail, %d fields)",
	       n_ok, n_fail, n_fields);
}

static const sysreg_field_map_t *lookup_sysreg_arg(const char *arg,
						   uint64_t *expected)
{
	const char *body;
	char name[160];
	char *eq;
	const sysreg_field_map_t *best = NULL;
	size_t best_len = 0;

	if (strncmp(arg, "SYSREG_", 7) != 0)
		return NULL;

	body = arg + 7;
	if (strlen(body) >= sizeof(name))
		return NULL;
	strcpy(name, body);

	eq = strchr(name, '=');
	if (!eq || eq == name)
		return NULL;
	*eq = '\0';
	*expected = strtoull(eq + 1, NULL, 0);

	for (const sysreg_field_map_t *m = sysreg_table; m->reg_name; m++) {
		char key[160];
		size_t len;

		snprintf(key, sizeof(key), "%s_%s", m->reg_name, m->field_name);
		len = strlen(key);
		if (strcasecmp(name, key) != 0)
			continue;
		if (len > best_len) {
			best = m;
			best_len = len;
		}
	}
	return best;
}

int main(int argc, char **argv)
{
	int tests_run = 0;
	int i;
	bool do_dump = false;

	report_prefix_push("idregs");

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "dump") == 0 ||
		    strcmp(argv[i], "dump-all") == 0)
			do_dump = true;
	}

	if (do_dump) {
		idregs_dump_all();
		report_prefix_pop();
		return report_summary();
	}

	for (i = 1; i < argc; i++) {
		uint64_t expected, reg_val, mask, actual;
		const sysreg_field_map_t *m;

		if (strncmp(argv[i], "SYSREG_", 7) != 0)
			continue;

		m = lookup_sysreg_arg(argv[i], &expected);
		if (!m) {
			report_fail("unknown SYSREG mapping for %s", argv[i]);
			continue;
		}

		if (!read_sysreg_by_name(m->reg_name, &reg_val)) {
			report_fail("MRS not implemented in test for %s", m->reg_name);
			continue;
		}

		tests_run++;
		mask = (m->width >= 64) ? ~0ULL : ((1ULL << m->width) - 1);
		actual = (reg_val >> m->shift) & mask;

		report(actual == expected,
		       "%s.%s: expected=0x%" PRIx64 " mrs=0x%" PRIx64
		       " (raw=0x%" PRIx64 ")",
		       m->reg_name, m->field_name, expected, actual, reg_val);
	}

	if (tests_run == 0)
		report_skip("No SYSREG_* test_args");

	report_prefix_pop();
	return report_summary();
}
