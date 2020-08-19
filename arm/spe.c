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
#include "asm/processor.h"
#include "alloc_page.h"
#include <bitops.h>

struct spe {
	int min_interval;
	int maxsize;
	int countsize;
	bool fl_cap;
	bool ft_cap;
	bool fe_cap;
	int align;
	void *buffer;
	bool unique_record_size;
};

static struct spe spe;

#ifdef __arm__

static bool spe_probe(void) { return false; }
static void test_spe_introspection(void) { }

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

#define PMSIDR_EL1	sys_reg(3, 0, 9, 9, 7)

#define PMBIDR_EL1	sys_reg(3, 0, 9, 10, 7)

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
	} else {
		report_abort("Unknown sub-test '%s'", argv[1]);
	}
	return report_summary();
}
