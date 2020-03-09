/*
 * GIC tests
 *
 * GICv2
 *   + test sending/receiving IPIs
 *   + MMIO access tests
 * GICv3
 *   + test sending/receiving IPIs
 *
 * Copyright (C) 2016, Red Hat Inc, Andrew Jones <drjones@redhat.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2.
 */
#include <libcflat.h>
#include <asm/setup.h>
#include <asm/processor.h>
#include <asm/delay.h>
#include <asm/gic.h>
#include <asm/gic-v3-its.h>
#include <asm/smp.h>
#include <asm/barrier.h>
#include <asm/io.h>

#define IPI_SENDER	1
#define IPI_IRQ		1

struct gic {
	struct {
		void (*send_self)(void);
		void (*send_broadcast)(void);
	} ipi;
};

static struct gic *gic;
static int acked[NR_CPUS], spurious[NR_CPUS];
static int bad_sender[NR_CPUS], bad_irq[NR_CPUS];
static cpumask_t ready;
typedef void (*handler_t)(struct pt_regs *regs __unused);

static void nr_cpu_check(int nr)
{
	if (nr_cpus < nr)
		report_abort("At least %d cpus required", nr);
}

static void wait_on_ready(void)
{
	cpumask_set_cpu(smp_processor_id(), &ready);
	while (!cpumask_full(&ready))
		cpu_relax();
}

static void stats_reset(void)
{
	int i;

	for (i = 0; i < nr_cpus; ++i) {
		acked[i] = 0;
		bad_sender[i] = -1;
		bad_irq[i] = -1;
	}
	smp_wmb();
}

static void check_acked(const char *testname, cpumask_t *mask)
{
	int missing = 0, extra = 0, unexpected = 0;
	int nr_pass, cpu, i;
	bool bad = false;

	/* Wait up to 5s for all interrupts to be delivered */
	for (i = 0; i < 50; ++i) {
		mdelay(100);
		nr_pass = 0;
		for_each_present_cpu(cpu) {
			smp_rmb();
			nr_pass += cpumask_test_cpu(cpu, mask) ?
				acked[cpu] == 1 : acked[cpu] == 0;

			if (bad_sender[cpu] != -1) {
				printf("cpu%d received IPI from wrong sender %d\n",
					cpu, bad_sender[cpu]);
				bad = true;
			}

			if (bad_irq[cpu] != -1) {
				printf("cpu%d received wrong irq %d\n",
					cpu, bad_irq[cpu]);
				bad = true;
			}
		}
		if (nr_pass == nr_cpus) {
			report(!bad, "%s", testname);
			if (i)
				report_info("took more than %d ms", i * 100);
			return;
		}
	}

	for_each_present_cpu(cpu) {
		if (cpumask_test_cpu(cpu, mask)) {
			if (!acked[cpu])
				++missing;
			else if (acked[cpu] > 1)
				++extra;
		} else {
			if (acked[cpu])
				++unexpected;
		}
	}

	report(false, "%s", testname);
	report_info("Timed-out (5s). ACKS: missing=%d extra=%d unexpected=%d",
		    missing, extra, unexpected);
}

static void check_spurious(void)
{
	int cpu;

	smp_rmb();
	for_each_present_cpu(cpu) {
		if (spurious[cpu])
			report_info("WARN: cpu%d got %d spurious interrupts",
				cpu, spurious[cpu]);
	}
}

static void check_ipi_sender(u32 irqstat)
{
	if (gic_version() == 2) {
		int src = (irqstat >> 10) & 7;

		if (src != IPI_SENDER)
			bad_sender[smp_processor_id()] = src;
	}
}

static void check_irqnr(u32 irqnr)
{
	if (irqnr != IPI_IRQ)
		bad_irq[smp_processor_id()] = irqnr;
}

static void ipi_handler(struct pt_regs *regs __unused)
{
	u32 irqstat = gic_read_iar();
	u32 irqnr = gic_iar_irqnr(irqstat);

	if (irqnr != GICC_INT_SPURIOUS) {
		gic_write_eoir(irqstat);
		smp_rmb(); /* pairs with wmb in stats_reset */
		++acked[smp_processor_id()];
		check_ipi_sender(irqstat);
		check_irqnr(irqnr);
		smp_wmb(); /* pairs with rmb in check_acked */
	} else {
		++spurious[smp_processor_id()];
		smp_wmb();
	}
}

static void setup_irq(handler_t handler)
{
	gic_enable_defaults();
#ifdef __arm__
	install_exception_handler(EXCPTN_IRQ, handler);
#else
	install_irq_handler(EL1H_IRQ, handler);
#endif
	local_irq_enable();
}

#if defined(__aarch64__)
struct its_event {
	int cpu_id;
	int lpi_id;
};

struct its_stats {
	struct its_event expected;
	struct its_event observed;
};

static struct its_stats lpi_stats;

static void lpi_handler(struct pt_regs *regs __unused)
{
	u32 irqstat = gic_read_iar();
	int irqnr = gic_iar_irqnr(irqstat);

	gic_write_eoir(irqstat);
	assert(irqnr >= 8192);
	smp_rmb(); /* pairs with wmb in lpi_stats_expect */
	lpi_stats.observed.cpu_id = smp_processor_id();
	lpi_stats.observed.lpi_id = irqnr;
	smp_wmb(); /* pairs with rmb in check_lpi_stats */
}

static void lpi_stats_expect(int exp_cpu_id, int exp_lpi_id)
{
	lpi_stats.expected.cpu_id = exp_cpu_id;
	lpi_stats.expected.lpi_id = exp_lpi_id;
	lpi_stats.observed.cpu_id = -1;
	lpi_stats.observed.lpi_id = -1;
	smp_wmb(); /* pairs with rmb in handler */
}

static void check_lpi_stats(const char *msg)
{
	bool pass = false;
	mdelay(100);
	smp_rmb(); /* pairs with wmb in lpi_handler */
	if (lpi_stats.observed.cpu_id != lpi_stats.expected.cpu_id ||
	    lpi_stats.observed.lpi_id != lpi_stats.expected.lpi_id) {
		if (lpi_stats.observed.cpu_id == -1 &&
		    lpi_stats.observed.lpi_id == -1) {
			report_info("No LPI received whereas (cpuid=%d, intid=%d) "
				    "was expected", lpi_stats.expected.cpu_id,
				    lpi_stats.expected.lpi_id);
		} else {
			report_info("Unexpected LPI (cpuid=%d, intid=%d)",
				    lpi_stats.observed.cpu_id,
				    lpi_stats.observed.lpi_id);
		}
	} else {
		pass = true;
	}
	report(pass, "%s", msg);
}

static void secondary_lpi_test(void)
{
	setup_irq(lpi_handler);
	cpumask_set_cpu(smp_processor_id(), &ready);
	while (1)
		wfi();
}
#endif

static void gicv2_ipi_send_self(void)
{
	writel(2 << 24 | IPI_IRQ, gicv2_dist_base() + GICD_SGIR);
}

static void gicv2_ipi_send_broadcast(void)
{
	writel(1 << 24 | IPI_IRQ, gicv2_dist_base() + GICD_SGIR);
}

static void gicv3_ipi_send_self(void)
{
	gic_ipi_send_single(IPI_IRQ, smp_processor_id());
}

static void gicv3_ipi_send_broadcast(void)
{
	gicv3_write_sgi1r(1ULL << 40 | IPI_IRQ << 24);
	isb();
}

static void ipi_test_self(void)
{
	cpumask_t mask;

	report_prefix_push("self");
	stats_reset();
	cpumask_clear(&mask);
	cpumask_set_cpu(smp_processor_id(), &mask);
	gic->ipi.send_self();
	check_acked("IPI: self", &mask);
	report_prefix_pop();
}

static void ipi_test_smp(void)
{
	cpumask_t mask;
	int i;

	report_prefix_push("target-list");
	stats_reset();
	cpumask_copy(&mask, &cpu_present_mask);
	for (i = smp_processor_id() & 1; i < nr_cpus; i += 2)
		cpumask_clear_cpu(i, &mask);
	gic_ipi_send_mask(IPI_IRQ, &mask);
	check_acked("IPI: directed", &mask);
	report_prefix_pop();

	report_prefix_push("broadcast");
	stats_reset();
	cpumask_copy(&mask, &cpu_present_mask);
	cpumask_clear_cpu(smp_processor_id(), &mask);
	gic->ipi.send_broadcast();
	check_acked("IPI: broadcast", &mask);
	report_prefix_pop();
}

static void ipi_send(void)
{
	setup_irq(ipi_handler);
	wait_on_ready();
	ipi_test_self();
	ipi_test_smp();
	check_spurious();
	exit(report_summary());
}

static void ipi_recv(void)
{
	setup_irq(ipi_handler);
	cpumask_set_cpu(smp_processor_id(), &ready);
	while (1)
		wfi();
}

static void ipi_test(void *data __unused)
{
	if (smp_processor_id() == IPI_SENDER)
		ipi_send();
	else
		ipi_recv();
}

static struct gic gicv2 = {
	.ipi = {
		.send_self = gicv2_ipi_send_self,
		.send_broadcast = gicv2_ipi_send_broadcast,
	},
};

static struct gic gicv3 = {
	.ipi = {
		.send_self = gicv3_ipi_send_self,
		.send_broadcast = gicv3_ipi_send_broadcast,
	},
};

static void ipi_clear_active_handler(struct pt_regs *regs __unused)
{
	u32 irqstat = gic_read_iar();
	u32 irqnr = gic_iar_irqnr(irqstat);

	if (irqnr != GICC_INT_SPURIOUS) {
		void *base;
		u32 val = 1 << IPI_IRQ;

		if (gic_version() == 2)
			base = gicv2_dist_base();
		else
			base = gicv3_sgi_base();

		writel(val, base + GICD_ICACTIVER);

		smp_rmb(); /* pairs with wmb in stats_reset */
		++acked[smp_processor_id()];
		check_irqnr(irqnr);
		smp_wmb(); /* pairs with rmb in check_acked */
	} else {
		++spurious[smp_processor_id()];
		smp_wmb();
	}
}

static void run_active_clear_test(void)
{
	report_prefix_push("active");
	setup_irq(ipi_clear_active_handler);
	ipi_test_self();
	report_prefix_pop();
}

static bool test_ro_pattern_32(void *address, u32 pattern, u32 orig)
{
	u32 reg;

	writel(pattern, address);
	reg = readl(address);

	if (reg != orig)
		writel(orig, address);

	return reg == orig;
}

static bool test_readonly_32(void *address, bool razwi)
{
	u32 orig, pattern;

	orig = readl(address);
	if (razwi && orig)
		return false;

	pattern = 0xffffffff;
	if (orig != pattern) {
		if (!test_ro_pattern_32(address, pattern, orig))
			return false;
	}

	pattern = 0xa5a55a5a;
	if (orig != pattern) {
		if (!test_ro_pattern_32(address, pattern, orig))
			return false;
	}

	pattern = 0;
	if (orig != pattern) {
		if (!test_ro_pattern_32(address, pattern, orig))
			return false;
	}

	return true;
}

static void test_typer_v2(uint32_t reg)
{
	int nr_gic_cpus = ((reg >> 5) & 0x7) + 1;

	report_info("nr_cpus=%d", nr_cpus);
	report(nr_cpus == nr_gic_cpus, "all CPUs have interrupts");
}

#define BYTE(reg32, byte) (((reg32) >> ((byte) * 8)) & 0xff)
#define REPLACE_BYTE(reg32, byte, new) (((reg32) & ~(0xff << ((byte) * 8))) |\
					((new) << ((byte) * 8)))

/*
 * Some registers are byte accessible, do a byte-wide read and write of known
 * content to check for this.
 * Apply a @mask to cater for special register properties.
 * @pattern contains the value already in the register.
 */
static void test_byte_access(void *base_addr, u32 pattern, u32 mask)
{
	u32 reg = readb(base_addr + 1);
	bool res;

	res = (reg == (BYTE(pattern, 1) & (mask >> 8)));
	report(res, "byte reads successful");
	if (!res)
		report_info("byte 1 of 0x%08x => 0x%02x", pattern & mask, reg);

	pattern = REPLACE_BYTE(pattern, 2, 0x1f);
	writeb(BYTE(pattern, 2), base_addr + 2);
	reg = readl(base_addr);
	res = (reg == (pattern & mask));
	report(res, "byte writes successful");
	if (!res)
		report_info("writing 0x%02x into bytes 2 => 0x%08x",
			    BYTE(pattern, 2), reg);
}

static void test_priorities(int nr_irqs, void *priptr)
{
	u32 orig_prio, reg, pri_bits;
	u32 pri_mask, pattern;
	void *first_spi = priptr + GIC_FIRST_SPI;

	orig_prio = readl(first_spi);
	report_prefix_push("IPRIORITYR");

	/*
	 * Determine implemented number of priority bits by writing all 1's
	 * and checking the number of cleared bits in the value read back.
	 */
	writel(0xffffffff, first_spi);
	pri_mask = readl(first_spi);

	reg = ~pri_mask;
	report((((reg >> 16) == (reg & 0xffff)) &&
	        ((reg & 0xff) == ((reg >> 8) & 0xff))),
	       "consistent priority masking");
	report_info("priority mask is 0x%08x", pri_mask);

	reg = reg & 0xff;
	for (pri_bits = 8; reg & 1; reg >>= 1, pri_bits--)
		;
	report(pri_bits >= 4, "implements at least 4 priority bits");
	report_info("%d priority bits implemented", pri_bits);

	pattern = 0;
	writel(pattern, first_spi);
	report(readl(first_spi) == pattern, "clearing priorities");

	/* setting all priorities to their max valus was tested above */

	report(test_readonly_32(priptr + nr_irqs, true),
	       "accesses beyond limit RAZ/WI");

	writel(pattern, priptr + nr_irqs - 4);
	report(readl(priptr + nr_irqs - 4) == (pattern & pri_mask),
	       "accessing last SPIs");

	pattern = 0xff7fbf3f;
	writel(pattern, first_spi);
	report(readl(first_spi) == (pattern & pri_mask),
	       "priorities are preserved");

	/* The PRIORITY registers are byte accessible. */
	test_byte_access(first_spi, pattern, pri_mask);

	report_prefix_pop();
	writel(orig_prio, first_spi);
}

/* GICD_ITARGETSR is only used by GICv2. */
static void test_targets(int nr_irqs)
{
	void *targetsptr = gicv2_dist_base() + GICD_ITARGETSR;
	u32 orig_targets;
	u32 cpu_mask;
	u32 pattern, reg;

	orig_targets = readl(targetsptr + GIC_FIRST_SPI);
	report_prefix_push("ITARGETSR");

	cpu_mask = (1 << nr_cpus) - 1;
	cpu_mask |= cpu_mask << 8;
	cpu_mask |= cpu_mask << 16;

	/* Check that bits for non implemented CPUs are RAZ/WI. */
	if (nr_cpus < 8) {
		writel(0xffffffff, targetsptr + GIC_FIRST_SPI);
		report(!(readl(targetsptr + GIC_FIRST_SPI) & ~cpu_mask),
		       "bits for non-existent CPUs masked");
		report_info("%d non-existent CPUs", 8 - nr_cpus);
	} else {
		report_skip("CPU masking (all CPUs implemented)");
	}

	report(test_readonly_32(targetsptr + nr_irqs, true),
	       "accesses beyond limit RAZ/WI");

	pattern = 0x0103020f;
	writel(pattern, targetsptr + GIC_FIRST_SPI);
	reg = readl(targetsptr + GIC_FIRST_SPI);
	report(reg == (pattern & cpu_mask), "register content preserved");
	if (reg != (pattern & cpu_mask))
		report_info("writing %08x reads back as %08x",
			    pattern & cpu_mask, reg);

	/* The TARGETS registers are byte accessible. */
	test_byte_access(targetsptr + GIC_FIRST_SPI, pattern, cpu_mask);

	writel(orig_targets, targetsptr + GIC_FIRST_SPI);

	report_prefix_pop();
}

static void gic_test_mmio(void)
{
	u32 reg;
	int nr_irqs;
	void *gic_dist_base, *idreg;

	switch(gic_version()) {
	case 0x2:
		gic_dist_base = gicv2_dist_base();
		idreg = gic_dist_base + GICD_ICPIDR2;
		break;
	case 0x3:
		report_abort("GICv3 MMIO tests NYI");
	default:
		report_abort("GIC version %d not supported", gic_version());
	}

	reg = readl(gic_dist_base + GICD_TYPER);
	nr_irqs = GICD_TYPER_IRQS(reg);
	report_info("number of implemented SPIs: %d", nr_irqs - GIC_FIRST_SPI);

	test_typer_v2(reg);

	report_info("IIDR: 0x%08x", readl(gic_dist_base + GICD_IIDR));

	report(test_readonly_32(gic_dist_base + GICD_TYPER, false),
               "GICD_TYPER is read-only");
	report(test_readonly_32(gic_dist_base + GICD_IIDR, false),
               "GICD_IIDR is read-only");

	reg = readl(idreg);
	report(test_readonly_32(idreg, false), "ICPIDR2 is read-only");
	report_info("value of ICPIDR2: 0x%08x", reg);

	test_priorities(nr_irqs, gic_dist_base + GICD_IPRIORITYR);

	if (gic_version() == 2)
		test_targets(nr_irqs);
}

#if defined(__aarch64__)

static void test_its_introspection(void)
{
	struct its_baser *dev_baser = &its_data.device_baser;
	struct its_baser *coll_baser = &its_data.coll_baser;
	struct its_typer *typer = &its_data.typer;

	if (!gicv3_its_base()) {
		report_skip("No ITS, skip ...");
		return;
	}

	/* IIDR */
	report(test_readonly_32(gicv3_its_base() + GITS_IIDR, false),
	       "GITS_IIDR is read-only"),

	/* TYPER */
	report(test_readonly_32(gicv3_its_base() + GITS_TYPER, false),
	       "GITS_TYPER is read-only");

	report(typer->phys_lpi, "ITS supports physical LPIs");
	report_info("vLPI support: %s", typer->virt_lpi ? "yes" : "no");
	report_info("ITT entry size = 0x%x", typer->ite_size);
	report_info("Bit Count: EventID=%d DeviceId=%d CollId=%d",
		    typer->eventid_bits, typer->deviceid_bits,
		    typer->collid_bits);
	report(typer->eventid_bits && typer->deviceid_bits &&
	       typer->collid_bits, "ID spaces");
	report_info("Target address format %s",
			typer->pta ? "Redist base address" : "PE #");

	report(dev_baser && coll_baser, "detect device and collection BASER");
	report_info("device table entry_size = 0x%x", dev_baser->esz);
	report_info("collection table entry_size = 0x%x", coll_baser->esz);
}

static int its_prerequisites(int nb_cpus)
{
	int cpu;

	if (!gicv3_its_base()) {
		report_skip("No ITS, skip ...");
		return -1;
	}

	if (nr_cpus < nb_cpus) {
		report_skip("Test requires at least %d vcpus", nb_cpus);
		return -1;
	}

	stats_reset();

	setup_irq(lpi_handler);

	for_each_present_cpu(cpu) {
		if (cpu == 0)
			continue;
		smp_boot_secondary(cpu, secondary_lpi_test);
	}
	wait_on_ready();

	its_enable_defaults();

	return 0;
}

static void test_its_trigger(void)
{
	struct its_collection *col3, *col2;
	struct its_device *dev2, *dev7;

	if (its_prerequisites(4))
		return;

	dev2 = its_create_device(2 /* dev id */, 8 /* nb_ites */);
	dev7 = its_create_device(7 /* dev id */, 8 /* nb_ites */);

	col3 = its_create_collection(3 /* col id */, 3/* target PE */);
	col2 = its_create_collection(2 /* col id */, 2/* target PE */);

	gicv3_lpi_set_config(8195, LPI_PROP_DEFAULT);
	gicv3_lpi_set_config(8196, LPI_PROP_DEFAULT);

	its_send_invall(col2);
	its_send_invall(col3);

	report_prefix_push("int");
	/*
	 * dev=2, eventid=20  -> lpi= 8195, col=3
	 * dev=7, eventid=255 -> lpi= 8196, col=2
	 * Trigger dev2, eventid=20 and dev7, eventid=255
	 * Check both LPIs hit
	 */

	its_send_mapd(dev2, true);
	its_send_mapd(dev7, true);

	its_send_mapc(col3, true);
	its_send_mapc(col2, true);

	its_send_mapti(dev2, 8195 /* lpi id */, 20 /* event id */, col3);
	its_send_mapti(dev7, 8196 /* lpi id */, 255 /* event id */, col2);

	lpi_stats_expect(3, 8195);
	its_send_int(dev2, 20);
	check_lpi_stats("dev=2, eventid=20  -> lpi= 8195, col=3");

	lpi_stats_expect(2, 8196);
	its_send_int(dev7, 255);
	check_lpi_stats("dev=7, eventid=255 -> lpi= 8196, col=2");

	report_prefix_pop();

	report_prefix_push("inv/invall");

	/*
	 * disable 8195, check dev2/eventid=20 does not trigger the
	 * corresponding LPI
	 */
	gicv3_lpi_set_config(8195, LPI_PROP_DEFAULT & ~LPI_PROP_ENABLED);
	its_send_inv(dev2, 20);

	lpi_stats_expect(-1, -1);
	its_send_int(dev2, 20);
	check_lpi_stats("dev2/eventid=20 does not trigger any LPI");

	/*
	 * re-enable the LPI but willingly do not call invall
	 * so the change in config is not taken into account.
	 * The LPI should not hit
	 */
	gicv3_lpi_set_config(8195, LPI_PROP_DEFAULT);
	lpi_stats_expect(-1, -1);
	its_send_int(dev2, 20);
	check_lpi_stats("dev2/eventid=20 still does not trigger any LPI");

	/* Now call the invall and check the LPI hits */
	its_send_invall(col3);
	lpi_stats_expect(3, 8195);
	its_send_int(dev2, 20);
	check_lpi_stats("dev2/eventid=20 now triggers an LPI");

	report_prefix_pop();

	report_prefix_push("mapd valid=false");
	/*
	 * Unmap device 2 and check the eventid 20 formerly
	 * attached to it does not hit anymore
	 */

	its_send_mapd(dev2, false);
	lpi_stats_expect(-1, -1);
	its_send_int(dev2, 20);
	check_lpi_stats("no LPI after device unmap");
	report_prefix_pop();

	/* Unmap the collection this time and check no LPI does hit */
	report_prefix_push("mapc valid=false");
	its_send_mapc(col2, false);
	lpi_stats_expect(-1, -1);
	its_send_int(dev7, 255);
	check_lpi_stats("no LPI after collection unmap");
	report_prefix_pop();
}
#endif

int main(int argc, char **argv)
{
	if (!gic_init()) {
		printf("No supported gic present, skipping tests...\n");
		return report_summary();
	}

	report_prefix_pushf("gicv%d", gic_version());

	switch (gic_version()) {
	case 2:
		gic = &gicv2;
		break;
	case 3:
		gic = &gicv3;
		break;
	}

	if (argc < 2)
		report_abort("no test specified");

	if (strcmp(argv[1], "ipi") == 0) {
		report_prefix_push(argv[1]);
		nr_cpu_check(2);
		on_cpus(ipi_test, NULL);
	} else if (strcmp(argv[1], "active") == 0) {
		run_active_clear_test();
	} else if (strcmp(argv[1], "mmio") == 0) {
		report_prefix_push(argv[1]);
		gic_test_mmio();
		report_prefix_pop();
	} else if (!strcmp(argv[1], "its-trigger")) {
		report_prefix_push(argv[1]);
		test_its_trigger();
		report_prefix_pop();
	} else if (strcmp(argv[1], "its-introspection") == 0) {
		report_prefix_push(argv[1]);
		test_its_introspection();
		report_prefix_pop();
	} else {
		report_abort("Unknown subtest '%s'", argv[1]);
	}

	return report_summary();
}
