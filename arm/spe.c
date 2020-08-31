/*
 * Copyright (C) 2020, Red Hat Inc, Eric Auger <eric.auger@redhat.com>
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
#include "asm/page.h"
#include "asm/processor.h"
#include "alloc_page.h"
#include <bitops.h>
#include "alloc.h"

struct spe {
	int min_interval;
	int maxsize;
	int countsize;
	bool fl_cap;
	bool ft_cap;
	bool fe_cap;
	int align;
	void *buffer;
	uint64_t pmbptr_el1;
	uint64_t pmblimitr_el1;
	uint64_t pmsirr_el1;
	uint64_t pmscr_el1;
	bool unique_record_size;
};

static struct spe spe;

#ifdef __arm__

static bool spe_probe(void) { return false; }
static void test_spe_introspection(void) { }
static void test_spe_buffer(void) { }

#else

#define ID_DFR0_PMSVER_SHIFT	32
#define ID_DFR0_PMSVER_MASK	0xF

#define PMBIDR_EL1_ALIGN_MASK	0xF
#define PMBIDR_EL1_P		0x10
#define PMBIDR_EL1_F		0x20

#define PMSIDR_EL1_FE		0x1
#define PMSIDR_EL1_FT		0x2
#define PMSIDR_EL1_FL		0x4
#define PMSIDR_EL1_ARCHINST	0x8
#define PMSIDR_EL1_LDS		0x10
#define PMSIDR_EL1_ERND		0x20
#define PMSIDR_EL1_INTERVAL_SHIFT	8
#define PMSIDR_EL1_INTERVAL_MASK	0xFUL
#define PMSIDR_EL1_MAXSIZE_SHIFT	12
#define PMSIDR_EL1_MAXSIZE_MASK		0xFUL
#define PMSIDR_EL1_COUNTSIZE_SHIFT	16
#define PMSIDR_EL1_COUNTSIZE_MASK	0xFUL

#define PMSIRR_EL1_INTERVAL_SHIFT	8
#define PMSIRR_EL1_INTERVAL_MASK	0xFFFFFF

#define PMSFCR_EL1_FE			0x1
#define PMSFCR_EL1_FT			0x2
#define PMSFCR_EL1_FL			0x4
#define PMSFCR_EL1_B			0x10000
#define PMSFCR_EL1_LD			0x20000
#define PMSFCR_EL1_ST			0x40000

#define PMSCR_EL1	sys_reg(3, 0, 9, 9, 0)
#define PMSICR_EL1	sys_reg(3, 0, 9, 9, 2)
#define PMSIRR_EL1	sys_reg(3, 0, 9, 9, 3)
#define PMSFCR_EL1	sys_reg(3, 0, 9, 9, 4)
#define PMSEVFR_EL1	sys_reg(3, 0, 9, 9, 5)
#define PMSIDR_EL1	sys_reg(3, 0, 9, 9, 7)

#define PMBLIMITR_EL1	sys_reg(3, 0, 9, 10, 0)
#define PMBPTR_EL1	sys_reg(3, 0, 9, 10, 1)
#define PMBSR_EL1	sys_reg(3, 0, 9, 10, 3)
#define PMBIDR_EL1	sys_reg(3, 0, 9, 10, 7)

#define PMBLIMITR_EL1_E			0x1

#define PMSCR_EL1_E1SPE			0x2
#define PMSCR_EL1_PA			0x10
#define PMSCR_EL1_TS			0x20
#define PMSCR_EL1_PCT			0x40

static int min_interval(uint8_t idr_bits)
{
	switch (idr_bits) {
	case 0x0:
		return 256;
	case 0x2:
		return 512;
	case 0x3:
		return 768;
	case 0x4:
		return 1024;
	case 0x5:
		return 1536;
	case 0x6:
		return 2048;
	case 0x7:
		return 3072;
	case 0x8:
		return 4096;
	default:
		return -1;
	}
}

static bool spe_probe(void)
{
	uint64_t pmbidr_el1, pmsidr_el1;
	uint8_t pmsver;

	pmsver = (get_id_aa64dfr0() >> ID_DFR0_PMSVER_SHIFT) & ID_DFR0_PMSVER_MASK;

	report_info("PMSVer = %d", pmsver);
	if (!pmsver || pmsver > 2)
		return false;

	pmbidr_el1 = read_sysreg_s(PMBIDR_EL1);
	if (pmbidr_el1 & PMBIDR_EL1_P) {
		report_info("MBIDR_EL1: Profiling buffer owned by this exception level");
		return false;
	}

	spe.align = 1 << (pmbidr_el1 & PMBIDR_EL1_ALIGN_MASK);

	pmsidr_el1 = read_sysreg_s(PMSIDR_EL1);

	spe.min_interval = min_interval((pmsidr_el1 >> PMSIDR_EL1_INTERVAL_SHIFT) & PMSIDR_EL1_INTERVAL_MASK);
	spe.maxsize = 1 << ((pmsidr_el1 >> PMSIDR_EL1_MAXSIZE_SHIFT) & PMSIDR_EL1_MAXSIZE_MASK);
	spe.countsize = (pmsidr_el1 >> PMSIDR_EL1_COUNTSIZE_SHIFT) & PMSIDR_EL1_COUNTSIZE_MASK;

	spe.fl_cap = pmsidr_el1 & PMSIDR_EL1_FL;
	spe.ft_cap = pmsidr_el1 & PMSIDR_EL1_FT;
	spe.fe_cap = pmsidr_el1 & PMSIDR_EL1_FE;

	report_info("Align= %d bytes, Min Interval=%d Single record Max Size = %d bytes",
			spe.align, spe.min_interval, spe.maxsize);
	report_info("Filtering Caps: Lat=%d Type=%d Events=%d", spe.fl_cap, spe.ft_cap, spe.fe_cap);
	if (spe.align == spe.maxsize) {
		report_info("Each record is exactly %d bytes", spe.maxsize);
		spe.unique_record_size = true;
	}

	spe.buffer = alloc_pages(0);

	return true;
}

static void test_spe_introspection(void)
{
	report(spe.countsize == 0x2, "PMSIDR_EL1: CountSize = 0b0010");
	report(spe.maxsize >= 16 && spe.maxsize <= 2048,
		"PMSIDR_EL1: Single record max size = %d bytes", spe.maxsize);
	report(spe.min_interval >= 256 && spe.min_interval <= 4096,
		"PMSIDR_EL1: Minimal sampling interval = %d", spe.min_interval);
}

static void mem_access_loop(void *addr, int loop, uint64_t pmblimitr)
{
asm volatile(
	"	msr_s " xstr(PMBLIMITR_EL1) ", %[pmblimitr]\n"
	"       isb\n"
	"       mov     x10, %[loop]\n"
	"1:     sub     x10, x10, #1\n"
	"       ldr     x9, [%[addr]]\n"
	"       cmp     x10, #0x0\n"
	"       b.gt    1b\n"
	"	bfxil   %[pmblimitr], xzr, 0, 1\n"
	"	msr_s " xstr(PMBLIMITR_EL1) ", %[pmblimitr]\n"
	"       isb\n"
	:
	: [addr] "r" (addr), [pmblimitr] "r" (pmblimitr), [loop] "r" (loop)
	: "x8", "x9", "x10", "cc");
}

char null_buff[PAGE_SIZE] = {};

static void reset(void)
{
	/* erase the profiling buffer, reset the start and limit addresses */
	spe.pmbptr_el1 = (uint64_t)spe.buffer;
	spe.pmblimitr_el1 = (uint64_t)(spe.buffer + PAGE_SIZE);
	write_sysreg_s(spe.pmbptr_el1, PMBPTR_EL1);
	write_sysreg_s(spe.pmblimitr_el1, PMBLIMITR_EL1);
	isb();

	/* Drain any buffered data */
	psb_csync();
	dsb(nsh);

	memset(spe.buffer, 0, PAGE_SIZE);

	/* reset the syndrome register */
	write_sysreg_s(0, PMBSR_EL1);

	/* SW must write 0 to PMSICR_EL1 before enabling sampling profiling */
	write_sysreg_s(0, PMSICR_EL1);

	/* Filtering disabled */
	write_sysreg_s(0, PMSFCR_EL1);

	/* Interval Reload Register */
	spe.pmsirr_el1 = (spe.min_interval & PMSIRR_EL1_INTERVAL_MASK) << PMSIRR_EL1_INTERVAL_SHIFT;
	write_sysreg_s(spe.pmsirr_el1, PMSIRR_EL1);

	/* Control Registrer */
	spe.pmscr_el1 = PMSCR_EL1_E1SPE | PMSCR_EL1_TS | PMSCR_EL1_PCT | PMSCR_EL1_PA;
	write_sysreg_s(spe.pmscr_el1, PMSCR_EL1);

	/* Make sure the syndrome register is void */
	write_sysreg_s(0, PMBSR_EL1);
}

static inline void drain(void)
{
	/* ensure profiling data are written */
	psb_csync();
	dsb(nsh);
}

static void test_spe_buffer(void)
{
	uint64_t pmbsr_el1, val1, val2;
	void *addr = malloc(10 * PAGE_SIZE);

	reset();

	val1 = read_sysreg_s(PMBPTR_EL1);
	val2 = read_sysreg_s(PMBLIMITR_EL1);
	report(val1 == spe.pmbptr_el1 && val2 == spe.pmblimitr_el1,
	       "PMBPTR_EL1, PMBLIMITR_EL1: reset");

	val1 = read_sysreg_s(PMSIRR_EL1);
	report(val1 == spe.pmsirr_el1, "PMSIRR_EL1: Sampling interval set to %d", spe.min_interval);

	val1 = read_sysreg_s(PMSCR_EL1);
	report(val1 == spe.pmscr_el1, "PMSCR_EL1: EL1 Statistical Profiling enabled");

	val1 = read_sysreg_s(PMSFCR_EL1);
	report(!val1, "PMSFCR_EL1: No Filter Control");

	report(!memcmp(spe.buffer, null_buff, PAGE_SIZE),
		       "Profiling buffer empty before profiling");

	val1 = read_sysreg_s(PMBSR_EL1);
	report(!val1, "PMBSR_EL1: Syndrome Register void before profiling");

	mem_access_loop(addr, 1, spe.pmblimitr_el1 | PMBLIMITR_EL1_E);
	drain();
	val1 = read_sysreg_s(PMSICR_EL1);
	/*
	 * TODO: the value read in PMSICR_EL1.count currently seems not consistent with
	 * programmed interval. Getting a good value would allow to estimate the number
	 * of records to be collected in next step.
	 */
	report_info("count for a single iteration: PMSICR_EL1.count=%lld interval=%d",
		    val1 & GENMASK_ULL(31, 0), spe.min_interval);

	/* Stuff to profile */

	mem_access_loop(addr, 1000000, spe.pmblimitr_el1 | PMBLIMITR_EL1_E);

	/* end of stuff to profile */

	drain();

	report(memcmp(spe.buffer, null_buff, PAGE_SIZE), "Profiling buffer filled");

	val1 = read_sysreg_s(PMBPTR_EL1);
	val2 = val1 - (uint64_t)spe.buffer;
	report(val1 > (uint64_t)spe.buffer,
		"PMBPTR_EL1: Current write position has increased: 0x%lx -> 0x%lx (%ld bytes)",
		(uint64_t)spe.buffer, val1, val2);
	if (spe.unique_record_size)
		report_info("This corresponds to %ld record(s) of %d bytes",
			    val2 / spe.maxsize, spe.maxsize);
	pmbsr_el1 = read_sysreg_s(PMBSR_EL1);
	report(!pmbsr_el1, "PMBSR_EL1: no event");

	free(addr);
}

#endif

int main(int argc, char *argv[])
{
	if (!spe_probe()) {
		printf("SPE not supported, test skipped...\n");
		return report_summary();
	}

	if (argc < 2)
		report_abort("no test specified");

	report_prefix_push("spe");

	if (strcmp(argv[1], "spe-introspection") == 0) {
		report_prefix_push(argv[1]);
		test_spe_introspection();
		report_prefix_pop();
	} else if (strcmp(argv[1], "spe-buffer") == 0) {
		report_prefix_push(argv[1]);
		test_spe_buffer();
		report_prefix_pop();
	} else {
		report_abort("Unknown sub-test '%s'", argv[1]);
	}
	return report_summary();
}
