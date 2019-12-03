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
#include <asm/gic.h>

#define PMU_PMCR_E         (1 << 0)
#define PMU_PMCR_P         (1 << 1)
#define PMU_PMCR_C         (1 << 2)
#define PMU_PMCR_D         (1 << 3)
#define PMU_PMCR_X         (1 << 4)
#define PMU_PMCR_DP        (1 << 5)
#define PMU_PMCR_LC        (1 << 6)
#define PMU_PMCR_N_SHIFT   11
#define PMU_PMCR_N_MASK    0x1f
#define PMU_PMCR_ID_SHIFT  16
#define PMU_PMCR_ID_MASK   0xff
#define PMU_PMCR_IMP_SHIFT 24
#define PMU_PMCR_IMP_MASK  0xff

#define PMU_CYCLE_IDX      31

#define NR_SAMPLES 10

struct pmu {
	unsigned int version;
	unsigned int nb_implemented_counters;
	uint32_t pmcr_ro;
};

static struct pmu pmu;


#if defined(__arm__)
#define ID_DFR0_PERFMON_SHIFT 24
#define ID_DFR0_PERFMON_MASK  0xf

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
static void test_event_counter_config(void) {}
static void test_basic_event_count(void) {}
static void test_mem_access(void) {}
static void test_chained_counters(void) {}
static void test_chained_sw_incr(void) {}

#elif defined(__aarch64__)
#define ID_AA64DFR0_PERFMON_SHIFT 8
#define ID_AA64DFR0_PERFMON_MASK  0xf

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
	return ver == 1 ? 3 : ver;
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

#define PMCEID1_EL0 sys_reg(11, 3, 9, 12, 7)
#define PMCNTENSET_EL0 sys_reg(11, 3, 9, 12, 1)
#define PMCNTENCLR_EL0 sys_reg(11, 3, 9, 12, 2)

#define PMEVTYPER_EXCLUDE_EL1 (1 << 31)
#define PMEVTYPER_EXCLUDE_EL0 (1 << 30)

#define regn_el0(__reg, __n) __reg ## __n  ## _el0
#define write_regn(__reg, __n, __val) \
	write_sysreg((__val), __reg ## __n ## _el0)

#define read_regn(__reg, __n) \
	read_sysreg(__reg ## __n ## _el0)

#define print_pmevtyper(__s , __n) do { \
	uint32_t val; \
	val = read_regn(pmevtyper, __n);\
	report_info("%s pmevtyper%d=0x%x, eventcount=0x%x (p=%ld, u=%ld nsk=%ld, nsu=%ld, nsh=%ld m=%ld, mt=%ld)", \
			(__s), (__n), val, val & 0xFFFF,  \
			(BIT_MASK(31) & val) >> 31, \
			(BIT_MASK(30) & val) >> 30, \
			(BIT_MASK(29) & val) >> 29, \
			(BIT_MASK(28) & val) >> 28, \
			(BIT_MASK(27) & val) >> 27, \
			(BIT_MASK(26) & val) >> 26, \
			(BIT_MASK(25) & val) >> 25); \
	} while (0)

static bool is_event_supported(uint32_t n, bool warn)
{
	uint64_t pmceid0 = read_sysreg(pmceid0_el0);
	uint64_t pmceid1 = read_sysreg_s(PMCEID1_EL0);
	bool supported;
	uint32_t reg;

	if (n >= 0x0  && n <= 0x1F) {
		reg = pmceid0 & 0xFFFFFFFF;
	} else if  (n >= 0x4000 && n <= 0x401F) {
		reg = pmceid0 >> 32;
	} else if (n >= 0x20  && n <= 0x3F) {
		reg = pmceid1 & 0xFFFFFFFF;
	} else if (n >= 0x4020 && n <= 0x403F) {
		reg = pmceid1 >> 32;
	} else {
		abort();
	}
	supported =  reg & (1 << n);
	if (!supported && warn)
		report_info("event %d is not supported", n);
	return supported;
}

static void test_event_introspection(void)
{
	bool required_events;

	if (!pmu.nb_implemented_counters) {
		report_skip("No event counter, skip ...");
		return;
	}
	if (pmu.nb_implemented_counters < 2)
		report_info("%d event counters are implemented. "
                            "ARM recommends to implement at least 2",
                            pmu.nb_implemented_counters);

	/* PMUv3 requires an implementation includes some common events */
	required_events = is_event_supported(0x0, true) /* SW_INCR */ &&
			  is_event_supported(0x11, true) /* CPU_CYCLES */ &&
			  (is_event_supported(0x8, true) /* INST_RETIRED */ ||
			   is_event_supported(0x1B, true) /* INST_PREC */);
	if (!is_event_supported(0x8, false))
		report_info("ARM strongly recomments INST_RETIRED (0x8) event "
			    "to be implemented");

	if (pmu.version == 0x4) {
		/* ARMv8.1 PMU: STALL_FRONTEND and STALL_BACKEND are required */
		required_events = required_events ||
				  is_event_supported(0x23, true) ||
				  is_event_supported(0x24, true);
	}

	/* L1D_CACHE_REFILL(0x3) and L1D_CACHE(0x4) are only required if
	   L1 data / unified cache. BR_MIS_PRED(0x10), BR_PRED(0x12) are only
	   required if program-flow prediction is implemented. */

	report("Check required events are implemented", required_events);
}

static inline void mem_access_loop(void *addr, int loop, uint32_t pmcr)
{
asm volatile(
	"       msr     pmcr_el0, %[pmcr]\n"
	"       isb\n"
	"       mov     x10, %[loop]\n"
	"1:     sub     x10, x10, #1\n"
	"       mov x8, %[addr]\n"
	"       ldr x9, [x8]\n"
	"       cmp     x10, #0x0\n"
	"       b.gt    1b\n"
	"       msr     pmcr_el0, xzr\n"
	"       isb\n"
	:
	: [addr] "r" (addr), [pmcr] "r" (pmcr), [loop] "r" (loop)
	: );
}


static void pmu_reset(void)
{
	/* reset all counters, counting disabled at PMCR level*/
	set_pmcr(pmu.pmcr_ro | PMU_PMCR_LC | PMU_PMCR_C | PMU_PMCR_P);
	/* Disable all counters */
	write_sysreg_s(0xFFFFFFFF, PMCNTENCLR_EL0);
	/* clear overflow reg */
	write_sysreg(0xFFFFFFFF, pmovsclr_el0);
	/* disable overflow interrupts on all counters */
	write_sysreg(0xFFFFFFFF, pmintenclr_el1);
	isb();
}

static void test_event_counter_config(void) {
	int i;

	if (!pmu.nb_implemented_counters) {
		report_skip("No event counter, skip ...");
		return;
	}

	pmu_reset();

	/* Test setting through PMESELR/PMXEVTYPER and PMEVTYPERn read */
        /* select counter 0 */
        write_sysreg(1, PMSELR_EL0);
        /* program this counter to count unsupported event */
        write_sysreg(0xEA, PMXEVTYPER_EL0);
        write_sysreg(0xdeadbeef, PMXEVCNTR_EL0);
	report("PMESELR/PMXEVTYPER/PMEVTYPERn",
		(read_regn(pmevtyper, 1) & 0xFFF) == 0xEA);
	report("PMESELR/PMXEVCNTR/PMEVCNTRn",
		(read_regn(pmevcntr, 1) == 0xdeadbeef));

	/* try configure an unsupported event within the range [0x0, 0x3F] */
	for (i = 0; i <= 0x3F; i++) {
		if (!is_event_supported(i, false))
			goto test_unsupported;
	}
	report_skip("pmevtyper: all events within [0x0, 0x3F] are supported");

test_unsupported:
	/* select counter 0 */
	write_sysreg(0, PMSELR_EL0);
	/* program this counter to count unsupported event */
	write_sysreg(i, PMXEVCNTR_EL0);
	/* read the counter value */
	read_sysreg(PMXEVCNTR_EL0);
	report("read of a counter programmed with unsupported event", read_sysreg(PMXEVCNTR_EL0) == i);

}

static bool satisfy_prerequisites(uint32_t *events, unsigned int nb_events)
{
	int i;

	if (pmu.nb_implemented_counters < nb_events) {
		report_skip("Skip test as number of counters is too small (%d)",
			    pmu.nb_implemented_counters);
		return false;
	}

	for (i = 0; i < nb_events; i++) {
		if (!is_event_supported(events[i], false)) {
			report_skip("Skip test as event %d is not supported",
				    events[i]);
			return false;
		}
	}		
	return true;
}

static void test_basic_event_count(void)
{
	uint32_t implemented_counter_mask, non_implemented_counter_mask;
	uint32_t counter_mask;
	uint32_t events[] = {
		0x11,	/* CPU_CYCLES */
		0x8,	/* INST_RETIRED */
	};

	if (!satisfy_prerequisites(events, ARRAY_SIZE(events)))
		return;

	implemented_counter_mask = (1 << pmu.nb_implemented_counters) - 1;
	non_implemented_counter_mask = ~((1 << 31) | implemented_counter_mask);
	counter_mask = implemented_counter_mask | non_implemented_counter_mask;

        write_regn(pmevtyper, 0, events[0] | PMEVTYPER_EXCLUDE_EL0);
        write_regn(pmevtyper, 1, events[1] | PMEVTYPER_EXCLUDE_EL0);

	/* disable all counters */
	write_sysreg_s(0xFFFFFFFF, PMCNTENCLR_EL0);
	report("pmcntenclr: disable all counters",
	       !read_sysreg_s(PMCNTENCLR_EL0) && !read_sysreg_s(PMCNTENSET_EL0));

	/*
	 * clear cycle and all event counters and allow counter enablement
	 * through PMCNTENSET. LC is RES1.
	 */
	set_pmcr(pmu.pmcr_ro | PMU_PMCR_LC | PMU_PMCR_C | PMU_PMCR_P);
	isb();	
	report("pmcr: reset counters", get_pmcr() == (pmu.pmcr_ro | PMU_PMCR_LC));

	/* Preset counter #0 to 0xFFFFFFF0 to trigger an overflow interrupt */
	write_regn(pmevcntr, 0, 0xFFFFFFF0);
	report("counter #0 preset to 0xFFFFFFF0",
		read_regn(pmevcntr, 0) == 0xFFFFFFF0);
	report("counter #1 is 0", !read_regn(pmevcntr, 1));

	/*
	 * Enable all implemented counters and also attempt to enable
	 * not supported counters. Counting still is disabled by !PMCR.E
	 */
	write_sysreg_s(counter_mask, PMCNTENSET_EL0);

	/* check only those implemented are enabled */
	report("pmcntenset: enabled implemented_counters",
	       (read_sysreg_s(PMCNTENSET_EL0) == read_sysreg_s(PMCNTENCLR_EL0)) &&
		(read_sysreg_s(PMCNTENSET_EL0) == implemented_counter_mask));

	/* Disable all counters but counters #0 and #1 */
	write_sysreg_s(~0x3, PMCNTENCLR_EL0);
	report("pmcntenset: just enabled #0 and #1",
	       (read_sysreg_s(PMCNTENSET_EL0) == read_sysreg_s(PMCNTENCLR_EL0)) &&
		(read_sysreg_s(PMCNTENSET_EL0) == 0x3));

	/* clear overflow register */
	write_sysreg(0xFFFFFFFF, pmovsclr_el0);
	report("check overflow reg is 0", !read_sysreg(pmovsclr_el0));

	/* disable overflow interrupts on all counters*/
	write_sysreg(0xFFFFFFFF, pmintenclr_el1);
	report("pmintenclr_el1=0, all interrupts disabled",
		!read_sysreg(pmintenclr_el1));

	/* enable overflow interrupts on all event counters */
	write_sysreg(implemented_counter_mask | non_implemented_counter_mask,
		     pmintenset_el1);
	report("overflow interrupts enabled on all implemented counters",
		read_sysreg(pmintenset_el1) == implemented_counter_mask);

	/* Set PMCR.E, execute asm code and unset PMCR.E */
	precise_instrs_loop(20, pmu.pmcr_ro | PMU_PMCR_E);

	report_info("counter #0 is 0x%lx (CPU_CYCLES)", read_regn(pmevcntr, 0));
	report_info("counter #1 is 0x%lx (INST_RETIRED)", read_regn(pmevcntr, 1));

	report_info("overflow reg = 0x%lx", read_sysreg(pmovsclr_el0) );
	report("check overflow happened on #0 only", read_sysreg(pmovsclr_el0) & 0x1);
}

static void test_mem_access(void)
{
	void *addr = malloc(PAGE_SIZE);
	uint32_t events[] = {
                0x13,   /* MEM_ACCESS */
                0x13,   /* MEM_ACCESS */
        };

	if (!satisfy_prerequisites(events, ARRAY_SIZE(events)))
		return;

	pmu_reset();

        write_regn(pmevtyper, 0, events[0] | PMEVTYPER_EXCLUDE_EL0);
        write_regn(pmevtyper, 1, events[1] | PMEVTYPER_EXCLUDE_EL0);
	write_sysreg_s(0x3, PMCNTENSET_EL0);
	isb();
	mem_access_loop(addr, 20, pmu.pmcr_ro | PMU_PMCR_E);
	report_info("counter #0 is %ld (MEM_ACCESS)", read_regn(pmevcntr, 0));
	report_info("counter #1 is %ld (MEM_ACCESS)", read_regn(pmevcntr, 1));
	/* We may not measure exactly 20 mem access, this depends on the platform */
	report("Ran 20 mem accesses",
	       (read_regn(pmevcntr, 0) == read_regn(pmevcntr, 1)) &&
	       (read_regn(pmevcntr, 0) >= 20) && !read_sysreg(pmovsclr_el0));

	pmu_reset();

	write_regn(pmevcntr, 0, 0xFFFFFFFA);
	write_regn(pmevcntr, 1, 0xFFFFFFF0);
	write_sysreg_s(0x3, PMCNTENSET_EL0);
	isb();
	mem_access_loop(addr, 20, pmu.pmcr_ro | PMU_PMCR_E);
	report("Ran 20 mem accesses with expected overflows on both counters",
	       read_sysreg(pmovsclr_el0) == 0x3);
	report_info("cnt#0 = %ld cnt#1=%ld overflow=0x%lx",
			read_regn(pmevcntr, 0), read_regn(pmevcntr, 1),
			read_sysreg(pmovsclr_el0));
}

static void test_chained_counters(void)
{
	uint32_t events[] = { 0x11 /* CPU_CYCLES */, 0x1E /* CHAIN */};

	if (!satisfy_prerequisites(events, ARRAY_SIZE(events)))
		return;

	pmu_reset();

        write_regn(pmevtyper, 0, events[0] | PMEVTYPER_EXCLUDE_EL0);
        write_regn(pmevtyper, 1, events[1] | PMEVTYPER_EXCLUDE_EL0);
	/* enable counters #0 and #1 */
	write_sysreg_s(0x3, PMCNTENSET_EL0);
	/* preset counter #0 at 0xFFFFFFF0 */
	write_regn(pmevcntr, 0, 0xFFFFFFF0);

	precise_instrs_loop(22, pmu.pmcr_ro | PMU_PMCR_E);

	report("CHAIN counter #1 incremented", read_regn(pmevcntr, 1) == 1); 
	report("check no overflow is recorded", !read_sysreg(pmovsclr_el0));

	/* test 64b overflow */

	pmu_reset();
	write_sysreg_s(0x3, PMCNTENSET_EL0);

	write_regn(pmevcntr, 0, 0xFFFFFFF0);
	write_regn(pmevcntr, 1, 0x1);
	precise_instrs_loop(22, pmu.pmcr_ro | PMU_PMCR_E);
	report_info("overflow reg = 0x%lx", read_sysreg(pmovsclr_el0));
	report("CHAIN counter #1 incremented", read_regn(pmevcntr, 1) == 2); 
	report("check no overflow is recorded", !read_sysreg(pmovsclr_el0));

	write_regn(pmevcntr, 0, 0xFFFFFFF0);
	write_regn(pmevcntr, 1, 0xFFFFFFFF);

	precise_instrs_loop(22, pmu.pmcr_ro | PMU_PMCR_E);
	report_info("overflow reg = 0x%lx", read_sysreg(pmovsclr_el0));
	report("CHAIN counter #1 wrapped", !read_regn(pmevcntr, 1)); 
	report("check no overflow is recorded", read_sysreg(pmovsclr_el0) == 0x2);
}

static void test_chained_sw_incr(void)
{
	uint32_t events[] = { 0x0 /* SW_INCR */, 0x0 /* SW_INCR */};
	int i;

	if (!satisfy_prerequisites(events, ARRAY_SIZE(events)))
		return;

	pmu_reset();

        write_regn(pmevtyper, 0, events[0] | PMEVTYPER_EXCLUDE_EL0);
        write_regn(pmevtyper, 1, events[1] | PMEVTYPER_EXCLUDE_EL0);
	/* enable counters #0 and #1 */
	write_sysreg_s(0x3, PMCNTENSET_EL0);

	/* preset counter #0 at 0xFFFFFFF0 */
	write_regn(pmevcntr, 0, 0xFFFFFFF0);

	for (i = 0; i < 100; i++) {
		write_sysreg(0x1, pmswinc_el0);
	}
	report_info("SW_INCR counter #0 has value %ld", read_regn(pmevcntr, 0)); 
	report("PWSYNC does not increment if PMCR.E is unset",
		read_regn(pmevcntr, 0) == 0xFFFFFFF0);

	pmu_reset();

	write_regn(pmevcntr, 0, 0xFFFFFFF0);
	write_sysreg_s(0x3, PMCNTENSET_EL0);
	set_pmcr(pmu.pmcr_ro | PMU_PMCR_E);

	for (i = 0; i < 100; i++) {
		write_sysreg(0x3, pmswinc_el0);
	}
	report("counter #1 after + 100 SW_INCR", read_regn(pmevcntr, 0)  == 84);
	report("counter #0 after + 100 SW_INCR", read_regn(pmevcntr, 1)  == 100);
	report_info(" counter values after 100 SW_INCR #0=%ld #1=%ld",
			read_regn(pmevcntr, 0), read_regn(pmevcntr, 1));
	report("overflow reg after 100 SW_INCR", read_sysreg(pmovsclr_el0) == 0x1);

	/* 64b SW_INCR */
	pmu_reset();

	events[1] = 0x1E /* CHAIN */;
        write_regn(pmevtyper, 1, events[1] | PMEVTYPER_EXCLUDE_EL0);
	write_regn(pmevcntr, 0, 0xFFFFFFF0);
	write_sysreg_s(0x3, PMCNTENSET_EL0);
	set_pmcr(pmu.pmcr_ro | PMU_PMCR_E);
	for (i = 0; i < 100; i++) {
		write_sysreg(0x3, pmswinc_el0);
	}
	report("overflow reg after 100 SW_INCR/CHAIN",
		!read_sysreg(pmovsclr_el0) && (read_regn(pmevcntr, 1) == 1));
	report_info("overflow=0x%lx, #0=%ld #1=%ld", read_sysreg(pmovsclr_el0),
		    read_regn(pmevcntr, 0), read_regn(pmevcntr, 1));

	/* 64b SW_INCR and overflow on CHAIN counter*/
	pmu_reset();

        write_regn(pmevtyper, 1, events[1] | PMEVTYPER_EXCLUDE_EL0);
	write_regn(pmevcntr, 0, 0xFFFFFFF0);
	write_regn(pmevcntr, 1, 0xFFFFFFFF);
	write_sysreg_s(0x3, PMCNTENSET_EL0);
	set_pmcr(pmu.pmcr_ro | PMU_PMCR_E);
	for (i = 0; i < 100; i++) {
		write_sysreg(0x3, pmswinc_el0);
	}
	report("overflow reg after 100 SW_INCR/CHAIN",
		(read_sysreg(pmovsclr_el0) == 0x2) &&
		(read_regn(pmevcntr, 1) == 0) &&
		(read_regn(pmevcntr, 0) == 84));
	report_info("overflow=0x%lx, #0=%ld #1=%ld", read_sysreg(pmovsclr_el0),
		    read_regn(pmevcntr, 0), read_regn(pmevcntr, 1));
}

#endif

/*
 * As a simple sanity check on the PMCR_EL0, ensure the implementer field isn't
 * null. Also print out a couple other interesting fields for diagnostic
 * purposes. For example, as of fall 2016, QEMU TCG mode doesn't implement
 * event counters and therefore reports zero event counters, but hopefully
 * support for at least the instructions event will be added in the future and
 * the reported number of event counters will become nonzero.
 */
static bool check_pmcr(void)
{
	uint32_t pmcr;

	pmcr = get_pmcr();

	report_info("PMU implementer/ID code/counters: %#x(\"%c\")/%#x/%d",
		    (pmcr >> PMU_PMCR_IMP_SHIFT) & PMU_PMCR_IMP_MASK,
		    ((pmcr >> PMU_PMCR_IMP_SHIFT) & PMU_PMCR_IMP_MASK) ? : ' ',
		    (pmcr >> PMU_PMCR_ID_SHIFT) & PMU_PMCR_ID_MASK,
		    (pmcr >> PMU_PMCR_N_SHIFT) & PMU_PMCR_N_MASK);

	return ((pmcr >> PMU_PMCR_IMP_SHIFT) & PMU_PMCR_IMP_MASK) != 0;
}

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
	if (pmu.version == 0x3) {
		if (ERRATA(9e3f7a296940)) {
			write_sysreg(0xdead, PMCCNTR64);
			report("pmccntr64", read_sysreg(PMCCNTR64) == 0xdead);
		} else
			report_skip("Skipping unsafe pmccntr64 test. Set ERRATA_9e3f7a296940=y to enable.");
	}
#endif
}

/* Return FALSE if no PMU found, otherwise return TRUE */
static bool pmu_probe(void)
{
	uint32_t pmcr;

	pmu.version = get_pmu_version();
	report_info("PMU version: %d", pmu.version);

	if (pmu.version == 0 || pmu.version  == 0xF)
		return false;

	pmcr = get_pmcr();
	pmu.pmcr_ro = pmcr & 0xFFFFFF80;
	pmu.nb_implemented_counters = (pmcr >> PMU_PMCR_N_SHIFT) & PMU_PMCR_N_MASK;
	report_info("Implements %d event counters", pmu.nb_implemented_counters);

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
		report("Control register", check_pmcr());
		report("Monotonically increasing cycle count", check_cycles_increase());
		report("Cycle/instruction ratio", check_cpi(cpi));
		pmccntr64_test();
	} else if (strcmp(argv[1], "event-introspection") == 0) {
		report_prefix_push(argv[1]);
		test_event_introspection();
        } else if (strcmp(argv[1], "event-counter-config") == 0) {
                report_prefix_push(argv[1]);
		test_event_counter_config();
	} else if (strcmp(argv[1], "basic-event-count") == 0) {
		report_prefix_push(argv[1]);
		test_basic_event_count();
	} else if (strcmp(argv[1], "mem-access") == 0) {
		report_prefix_push(argv[1]);
		test_mem_access();
	} else if (strcmp(argv[1], "chained-counters") == 0) {
		report_prefix_push(argv[1]);
		test_chained_counters();
	} else if (strcmp(argv[1], "chained-sw-incr") == 0) {
		report_prefix_push(argv[1]);
		test_chained_sw_incr();
	} else {
		report_abort("Unknown subtest '%s'", argv[1]);
	}

	return report_summary();
}
