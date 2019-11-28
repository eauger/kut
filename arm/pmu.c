/*
 * Test the ARM Performance Monitors Unit (PMU).
 *
 * Copyright (c) 2015-2016, The Linux Foundation. All rights reserved.
 * Copyright (C) 2016, Red Hat Inc, Wei Huang <wei@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 and
 * only version 2.1 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License
 * for more details.
 */
#include "libcflat.h"
#include "errata.h"
#include "asm/barrier.h"
#include "asm/sysreg.h"
#include "asm/processor.h"
#include <bitops.h>

#define PMU_PMCR_E         (1 << 0)
#define PMU_PMCR_C         (1 << 2)
#define PMU_PMCR_LC        (1 << 6)
#define PMU_PMCR_N_SHIFT   11
#define PMU_PMCR_N_MASK    0x1f
#define PMU_PMCR_ID_SHIFT  16
#define PMU_PMCR_ID_MASK   0xff
#define PMU_PMCR_IMP_SHIFT 24
#define PMU_PMCR_IMP_MASK  0xff

#define PMU_CYCLE_IDX      31

#define NR_SAMPLES 10

/* Some PMU events */
#define SW_INCR			0x0
#define INST_RETIRED		0x8
#define CPU_CYCLES		0x11
#define INST_PREC		0x1B
#define STALL_FRONTEND		0x23
#define STALL_BACKEND		0x24

#define COMMON_EVENTS_LOW	0x0
#define COMMON_EVENTS_HIGH	0x3F
#define EXT_COMMON_EVENTS_LOW	0x4000
#define EXT_COMMON_EVENTS_HIGH	0x403F

struct pmu {
	unsigned int version;
	unsigned int nb_implemented_counters;
	uint32_t pmcr_ro;
};

static struct pmu pmu;

#if defined(__arm__)
#define ID_DFR0_PERFMON_SHIFT 24
#define ID_DFR0_PERFMON_MASK  0xf

#define ID_DFR0_PMU_NOTIMPL	0b0000
#define ID_DFR0_PMU_V1		0b0001
#define ID_DFR0_PMU_V2		0b0010
#define ID_DFR0_PMU_V3		0b0011
#define ID_DFR0_PMU_V3_8_1	0b0100
#define ID_DFR0_PMU_V3_8_4	0b0101
#define ID_DFR0_PMU_V3_8_5	0b0110
#define ID_DFR0_PMU_IMPDEF	0b1111

#define PMCR         __ACCESS_CP15(c9, 0, c12, 0)
#define ID_DFR0      __ACCESS_CP15(c0, 0, c1, 2)
#define PMSELR       __ACCESS_CP15(c9, 0, c12, 5)
#define PMXEVTYPER   __ACCESS_CP15(c9, 0, c13, 1)
#define PMCNTENSET   __ACCESS_CP15(c9, 0, c12, 1)
#define PMCCNTR32    __ACCESS_CP15(c9, 0, c13, 0)
#define PMCCNTR64    __ACCESS_CP15_64(0, c9)

static inline uint32_t get_id_dfr0(void) { return read_sysreg(ID_DFR0); }
static inline uint32_t get_pmcr(void) { return read_sysreg(PMCR); }
static inline void set_pmcr(uint32_t v) { write_sysreg(v, PMCR); }
static inline void set_pmcntenset(uint32_t v) { write_sysreg(v, PMCNTENSET); }

static inline uint8_t get_pmu_version(void)
{
	return (get_id_dfr0() >> ID_DFR0_PERFMON_SHIFT) & ID_DFR0_PERFMON_MASK;
}

static inline uint64_t get_pmccntr(void)
{
	return read_sysreg(PMCCNTR32);
}

static inline void set_pmccntr(uint64_t value)
{
	write_sysreg(value & 0xffffffff, PMCCNTR32);
}

/* PMCCFILTR is an obsolete name for PMXEVTYPER31 in ARMv7 */
static inline void set_pmccfiltr(uint32_t value)
{
	write_sysreg(PMU_CYCLE_IDX, PMSELR);
	write_sysreg(value, PMXEVTYPER);
	isb();
}

/*
 * Extra instructions inserted by the compiler would be difficult to compensate
 * for, so hand assemble everything between, and including, the PMCR accesses
 * to start and stop counting. isb instructions were inserted to make sure
 * pmccntr read after this function returns the exact instructions executed in
 * the controlled block. Total instrs = isb + mcr + 2*loop = 2 + 2*loop.
 */
static inline void precise_instrs_loop(int loop, uint32_t pmcr)
{
	asm volatile(
	"	mcr	p15, 0, %[pmcr], c9, c12, 0\n"
	"	isb\n"
	"1:	subs	%[loop], %[loop], #1\n"
	"	bgt	1b\n"
	"	mcr	p15, 0, %[z], c9, c12, 0\n"
	"	isb\n"
	: [loop] "+r" (loop)
	: [pmcr] "r" (pmcr), [z] "r" (0)
	: "cc");
}

/* event counter tests only implemented for aarch64 */
static void test_event_introspection(void) {}

#elif defined(__aarch64__)
#define ID_AA64DFR0_PERFMON_SHIFT 8
#define ID_AA64DFR0_PERFMON_MASK  0xf

#define ID_DFR0_PMU_NOTIMPL	0b0000
#define ID_DFR0_PMU_V3		0b0001
#define ID_DFR0_PMU_V3_8_1	0b0100
#define ID_DFR0_PMU_V3_8_4	0b0101
#define ID_DFR0_PMU_V3_8_5	0b0110
#define ID_DFR0_PMU_IMPDEF	0b1111

static inline uint32_t get_id_aa64dfr0(void) { return read_sysreg(id_aa64dfr0_el1); }
static inline uint32_t get_pmcr(void) { return read_sysreg(pmcr_el0); }
static inline void set_pmcr(uint32_t v) { write_sysreg(v, pmcr_el0); }
static inline uint64_t get_pmccntr(void) { return read_sysreg(pmccntr_el0); }
static inline void set_pmccntr(uint64_t v) { write_sysreg(v, pmccntr_el0); }
static inline void set_pmcntenset(uint32_t v) { write_sysreg(v, pmcntenset_el0); }
static inline void set_pmccfiltr(uint32_t v) { write_sysreg(v, pmccfiltr_el0); }

static inline uint8_t get_pmu_version(void)
{
	uint8_t ver = (get_id_aa64dfr0() >> ID_AA64DFR0_PERFMON_SHIFT) & ID_AA64DFR0_PERFMON_MASK;
	return ver;
}

/*
 * Extra instructions inserted by the compiler would be difficult to compensate
 * for, so hand assemble everything between, and including, the PMCR accesses
 * to start and stop counting. isb instructions are inserted to make sure
 * pmccntr read after this function returns the exact instructions executed
 * in the controlled block. Total instrs = isb + msr + 2*loop = 2 + 2*loop.
 */
static inline void precise_instrs_loop(int loop, uint32_t pmcr)
{
	asm volatile(
	"	msr	pmcr_el0, %[pmcr]\n"
	"	isb\n"
	"1:	subs	%[loop], %[loop], #1\n"
	"	b.gt	1b\n"
	"	msr	pmcr_el0, xzr\n"
	"	isb\n"
	: [loop] "+r" (loop)
	: [pmcr] "r" (pmcr)
	: "cc");
}

#define PMCEID1_EL0 sys_reg(3, 3, 9, 12, 7)

static bool is_event_supported(uint32_t n, bool warn)
{
	uint64_t pmceid0 = read_sysreg(pmceid0_el0);
	uint64_t pmceid1 = read_sysreg_s(PMCEID1_EL0);
	bool supported;
	uint64_t reg;

	/*
	 * The low 32-bits of PMCEID0/1 respectively describe
	 * event support for events 0-31/32-63. Their High
	 * 32-bits describe support for extended events
	 * starting at 0x4000, using the same split.
	 */
	assert((n >= COMMON_EVENTS_LOW  && n <= COMMON_EVENTS_HIGH) ||
	       (n >= EXT_COMMON_EVENTS_LOW && n <= EXT_COMMON_EVENTS_HIGH));

	if (n <= COMMON_EVENTS_HIGH)
		reg = lower_32_bits(pmceid0) | ((u64)lower_32_bits(pmceid1) << 32);
	else
		reg = upper_32_bits(pmceid0) | ((u64)upper_32_bits(pmceid1) << 32);

	supported =  reg & (1UL << (n & 0x3F));

	if (!supported && warn)
		report_info("event 0x%x is not supported", n);
	return supported;
}

static void test_event_introspection(void)
{
	bool required_events;

	if (!pmu.nb_implemented_counters) {
		report_skip("No event counter, skip ...");
		return;
	}

	/* PMUv3 requires an implementation includes some common events */
	required_events = is_event_supported(SW_INCR, true) &&
			  is_event_supported(CPU_CYCLES, true) &&
			  (is_event_supported(INST_RETIRED, true) ||
			   is_event_supported(INST_PREC, true));

	if (pmu.version >= ID_DFR0_PMU_V3_8_1) {
		required_events = required_events &&
				  is_event_supported(STALL_FRONTEND, true) &&
				  is_event_supported(STALL_BACKEND, true);
	}

	report(required_events, "Check required events are implemented");
}

#endif

/*
 * Ensure that the cycle counter progresses between back-to-back reads.
 */
static bool check_cycles_increase(void)
{
	bool success = true;

	/* init before event access, this test only cares about cycle count */
	set_pmcntenset(1 << PMU_CYCLE_IDX);
	set_pmccfiltr(0); /* count cycles in EL0, EL1, but not EL2 */

	set_pmcr(get_pmcr() | PMU_PMCR_LC | PMU_PMCR_C | PMU_PMCR_E);

	for (int i = 0; i < NR_SAMPLES; i++) {
		uint64_t a, b;

		a = get_pmccntr();
		b = get_pmccntr();

		if (a >= b) {
			printf("Read %"PRId64" then %"PRId64".\n", a, b);
			success = false;
			break;
		}
	}

	set_pmcr(get_pmcr() & ~PMU_PMCR_E);

	return success;
}

/*
 * Execute a known number of guest instructions. Only even instruction counts
 * greater than or equal to 4 are supported by the in-line assembly code. The
 * control register (PMCR_EL0) is initialized with the provided value (allowing
 * for example for the cycle counter or event counters to be reset). At the end
 * of the exact instruction loop, zero is written to PMCR_EL0 to disable
 * counting, allowing the cycle counter or event counters to be read at the
 * leisure of the calling code.
 */
static void measure_instrs(int num, uint32_t pmcr)
{
	int loop = (num - 2) / 2;

	assert(num >= 4 && ((num - 2) % 2 == 0));
	precise_instrs_loop(loop, pmcr);
}

/*
 * Measure cycle counts for various known instruction counts. Ensure that the
 * cycle counter progresses (similar to check_cycles_increase() but with more
 * instructions and using reset and stop controls). If supplied a positive,
 * nonzero CPI parameter, it also strictly checks that every measurement matches
 * it. Strict CPI checking is used to test -icount mode.
 */
static bool check_cpi(int cpi)
{
	uint32_t pmcr = get_pmcr() | PMU_PMCR_LC | PMU_PMCR_C | PMU_PMCR_E;

	/* init before event access, this test only cares about cycle count */
	set_pmcntenset(1 << PMU_CYCLE_IDX);
	set_pmccfiltr(0); /* count cycles in EL0, EL1, but not EL2 */

	if (cpi > 0)
		printf("Checking for CPI=%d.\n", cpi);
	printf("instrs : cycles0 cycles1 ...\n");

	for (unsigned int i = 4; i < 300; i += 32) {
		uint64_t avg, sum = 0;

		printf("%4d:", i);
		for (int j = 0; j < NR_SAMPLES; j++) {
			uint64_t cycles;

			set_pmccntr(0);
			measure_instrs(i, pmcr);
			cycles = get_pmccntr();
			printf(" %4"PRId64"", cycles);

			if (!cycles) {
				printf("\ncycles not incrementing!\n");
				return false;
			} else if (cpi > 0 && cycles != i * cpi) {
				printf("\nunexpected cycle count received!\n");
				return false;
			} else if ((cycles >> 32) != 0) {
				/* The cycles taken by the loop above should
				 * fit in 32 bits easily. We check the upper
				 * 32 bits of the cycle counter to make sure
				 * there is no supprise. */
				printf("\ncycle count bigger than 32bit!\n");
				return false;
			}

			sum += cycles;
		}
		avg = sum / NR_SAMPLES;
		printf(" avg=%-4"PRId64" %s=%-3"PRId64"\n", avg,
		       (avg >= i) ? "cpi" : "ipc",
		       (avg >= i) ? avg / i : i / avg);
	}

	return true;
}

static void pmccntr64_test(void)
{
#ifdef __arm__
	if (pmu.version == ID_DFR0_PMU_V3) {
		if (ERRATA(9e3f7a296940)) {
			write_sysreg(0xdead, PMCCNTR64);
			report(read_sysreg(PMCCNTR64) == 0xdead, "pmccntr64");
		} else
			report_skip("Skipping unsafe pmccntr64 test. Set ERRATA_9e3f7a296940=y to enable.");
	}
#endif
}

/* Return FALSE if no PMU found, otherwise return TRUE */
static bool pmu_probe(void)
{
	uint32_t pmcr = get_pmcr();

	pmu.version = get_pmu_version();
	if (pmu.version == ID_DFR0_PMU_NOTIMPL || pmu.version == ID_DFR0_PMU_IMPDEF)
		return false;

	report_info("PMU version: 0x%x", pmu.version);

	pmcr = get_pmcr();
	report_info("PMU implementer/ID code: %#x(\"%c\")/%#x",
		    (pmcr >> PMU_PMCR_IMP_SHIFT) & PMU_PMCR_IMP_MASK,
		    ((pmcr >> PMU_PMCR_IMP_SHIFT) & PMU_PMCR_IMP_MASK) ? : ' ',
		    (pmcr >> PMU_PMCR_ID_SHIFT) & PMU_PMCR_ID_MASK);

	/* store read-only and RES0 fields of the PMCR bottom-half*/
	pmu.pmcr_ro = pmcr & 0xFFFFFF00;
	pmu.nb_implemented_counters =
		(pmcr >> PMU_PMCR_N_SHIFT) & PMU_PMCR_N_MASK;
	report_info("Implements %d event counters",
		    pmu.nb_implemented_counters);

	return true;
}

int main(int argc, char *argv[])
{
	int cpi = 0;

	if (!pmu_probe()) {
		printf("No PMU found, test skipped...\n");
		return report_summary();
	}

	if (argc < 2)
		report_abort("no test specified");

	report_prefix_push("pmu");

	if (strcmp(argv[1], "cycle-counter") == 0) {
		report_prefix_push(argv[1]);
		if (argc > 2)
			cpi = atol(argv[2]);
		report(check_cycles_increase(),
		       "Monotonically increasing cycle count");
		report(check_cpi(cpi), "Cycle/instruction ratio");
		pmccntr64_test();
		report_prefix_pop();
	} else if (strcmp(argv[1], "pmu-event-introspection") == 0) {
		report_prefix_push(argv[1]);
		test_event_introspection();
		report_prefix_pop();
	} else {
		report_abort("Unknown sub-test '%s'", argv[1]);
	}

	return report_summary();
}
