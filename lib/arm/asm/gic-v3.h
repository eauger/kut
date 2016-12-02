/*
 * All GIC* defines are lifted from include/linux/irqchip/arm-gic-v3.h
 *
 * Copyright (C) 2016, Red Hat Inc, Andrew Jones <drjones@redhat.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2.
 */
#ifndef _ASMARM_GIC_V3_H_
#define _ASMARM_GIC_V3_H_

#ifndef _ASMARM_GIC_H_
#error Do not directly include <asm/gic-v3.h>. Include <asm/gic.h>
#endif

/*
 * Distributor registers
 *
 * We expect to be run in Non-secure mode, thus we define the
 * group1 enable bits with respect to that view.
 */
#define GICD_CTLR			0x0000
#define GICD_CTLR_RWP			(1U << 31)
#define GICD_CTLR_ARE_NS		(1U << 4)
#define GICD_CTLR_ENABLE_G1A		(1U << 1)
#define GICD_CTLR_ENABLE_G1		(1U << 0)

#define GICD_IROUTER			0x6000

/* Re-Distributor registers, offsets from RD_base */
#define GICR_TYPER			0x0008

#define GICR_TYPER_LAST			(1U << 4)

/* Re-Distributor registers, offsets from SGI_base */
#define GICR_IGROUPR0			GICD_IGROUPR
#define GICR_ISENABLER0			GICD_ISENABLER
#define GICR_IPRIORITYR0		GICD_IPRIORITYR

#define GICR_PROPBASER                  0x0070
#define GICR_PENDBASER                  0x0078
#define GICR_CTLR			GICD_CTLR
#define GICR_CTLR_ENABLE_LPIS		(1UL << 0)

#define ICC_SGI1R_AFFINITY_1_SHIFT	16
#define ICC_SGI1R_AFFINITY_2_SHIFT	32
#define ICC_SGI1R_AFFINITY_3_SHIFT	48
#define MPIDR_TO_SGI_AFFINITY(cluster_id, level) \
	(MPIDR_AFFINITY_LEVEL(cluster_id, level) << ICC_SGI1R_AFFINITY_## level ## _SHIFT)

#define GIC_BASER_CACHE_nCnB            0ULL
#define GIC_BASER_CACHE_SameAsInner     0ULL
#define GIC_BASER_CACHE_nC              1ULL
#define GIC_BASER_CACHE_RaWt            2ULL
#define GIC_BASER_CACHE_RaWb            3ULL
#define GIC_BASER_CACHE_WaWt            4ULL
#define GIC_BASER_CACHE_WaWb            5ULL
#define GIC_BASER_CACHE_RaWaWt          6ULL
#define GIC_BASER_CACHE_RaWaWb          7ULL
#define GIC_BASER_CACHE_MASK            7ULL
#define GIC_BASER_NonShareable          0ULL
#define GIC_BASER_InnerShareable        1ULL
#define GIC_BASER_OuterShareable        2ULL
#define GIC_BASER_SHAREABILITY_MASK     3ULL

#define GIC_BASER_CACHEABILITY(reg, inner_outer, type)                  \
	(GIC_BASER_CACHE_##type << reg##_##inner_outer##_CACHEABILITY_SHIFT)

#define GIC_BASER_SHAREABILITY(reg, type)                               \
	(GIC_BASER_##type << reg##_SHAREABILITY_SHIFT)

#define GICR_PROPBASER_SHAREABILITY_SHIFT               (10)
#define GICR_PROPBASER_INNER_CACHEABILITY_SHIFT         (7)
#define GICR_PROPBASER_OUTER_CACHEABILITY_SHIFT         (56)
#define GICR_PROPBASER_SHAREABILITY_MASK                                \
	GIC_BASER_SHAREABILITY(GICR_PROPBASER, SHAREABILITY_MASK)
#define GICR_PROPBASER_INNER_CACHEABILITY_MASK                          \
	GIC_BASER_CACHEABILITY(GICR_PROPBASER, INNER, MASK)
#define GICR_PROPBASER_OUTER_CACHEABILITY_MASK                          \
	GIC_BASER_CACHEABILITY(GICR_PROPBASER, OUTER, MASK)
#define GICR_PROPBASER_CACHEABILITY_MASK GICR_PROPBASER_INNER_CACHEABILITY_MASK

#define GICR_PROPBASER_InnerShareable                                   \
	GIC_BASER_SHAREABILITY(GICR_PROPBASER, InnerShareable)

#define GICR_PROPBASER_nCnB     GIC_BASER_CACHEABILITY(GICR_PROPBASER, INNER, nCnB)
#define GICR_PROPBASER_nC       GIC_BASER_CACHEABILITY(GICR_PROPBASER, INNER, nC)
#define GICR_PROPBASER_RaWt     GIC_BASER_CACHEABILITY(GICR_PROPBASER, INNER, RaWt)
#define GICR_PROPBASER_RaWb     GIC_BASER_CACHEABILITY(GICR_PROPBASER, INNER, RaWt)
#define GICR_PROPBASER_WaWt     GIC_BASER_CACHEABILITY(GICR_PROPBASER, INNER, WaWt)
#define GICR_PROPBASER_WaWb     GIC_BASER_CACHEABILITY(GICR_PROPBASER, INNER, WaWb)
#define GICR_PROPBASER_RaWaWt   GIC_BASER_CACHEABILITY(GICR_PROPBASER, INNER, RaWaWt)
#define GICR_PROPBASER_RaWaWb   GIC_BASER_CACHEABILITY(GICR_PROPBASER, INNER, RaWaWb)

#define GICR_PROPBASER_IDBITS_MASK                      (0x1f)

#define GICR_PENDBASER_SHAREABILITY_SHIFT               (10)
#define GICR_PENDBASER_INNER_CACHEABILITY_SHIFT         (7)
#define GICR_PENDBASER_OUTER_CACHEABILITY_SHIFT         (56)
#define GICR_PENDBASER_SHAREABILITY_MASK                                \
	GIC_BASER_SHAREABILITY(GICR_PENDBASER, SHAREABILITY_MASK)
#define GICR_PENDBASER_INNER_CACHEABILITY_MASK                          \
	GIC_BASER_CACHEABILITY(GICR_PENDBASER, INNER, MASK)
#define GICR_PENDBASER_OUTER_CACHEABILITY_MASK                          \
	GIC_BASER_CACHEABILITY(GICR_PENDBASER, OUTER, MASK)
#define GICR_PENDBASER_CACHEABILITY_MASK GICR_PENDBASER_INNER_CACHEABILITY_MASK

#define GICR_PENDBASER_InnerShareable                                   \
	GIC_BASER_SHAREABILITY(GICR_PENDBASER, InnerShareable)

#define GICR_PENDBASER_nCnB     GIC_BASER_CACHEABILITY(GICR_PENDBASER, INNER, nCnB)
#define GICR_PENDBASER_nC       GIC_BASER_CACHEABILITY(GICR_PENDBASER, INNER, nC)
#define GICR_PENDBASER_RaWt     GIC_BASER_CACHEABILITY(GICR_PENDBASER, INNER, RaWt)
#define GICR_PENDBASER_RaWb     GIC_BASER_CACHEABILITY(GICR_PENDBASER, INNER, RaWt)
#define GICR_PENDBASER_WaWt     GIC_BASER_CACHEABILITY(GICR_PENDBASER, INNER, WaWt)
#define GICR_PENDBASER_WaWb     GIC_BASER_CACHEABILITY(GICR_PENDBASER, INNER, WaWb)
#define GICR_PENDBASER_RaWaWt   GIC_BASER_CACHEABILITY(GICR_PENDBASER, INNER, RaWaWt)
#define GICR_PENDBASER_RaWaWb   GIC_BASER_CACHEABILITY(GICR_PENDBASER, INNER, RaWaWb)

#define GICR_PENDBASER_PTZ                              BIT_ULL(62)

#define LPI_PROP_GROUP1		(1 << 1)
#define LPI_PROP_ENABLED	(1 << 0)
#define LPI_PROP_DEFAULT_PRIO   0xa0
#define LPI_PROP_DEFAULT	(LPI_PROP_DEFAULT_PRIO |			\
				LPI_PROP_GROUP1 | LPI_PROP_ENABLED)

#include <asm/arch_gicv3.h>

#ifndef __ASSEMBLY__
#include <asm/setup.h>
#include <asm/processor.h>
#include <asm/delay.h>
#include <asm/cpumask.h>
#include <asm/smp.h>
#include <asm/io.h>

#define GICV3_NR_REDISTS 8

struct gicv3_data {
	void *dist_base;
	void *redist_bases[GICV3_NR_REDISTS];
	void *redist_base[NR_CPUS];
	void *lpi_prop;
	void *lpi_pend[NR_CPUS];
	unsigned int irq_nr;
};
extern struct gicv3_data gicv3_data;

#define gicv3_dist_base()		(gicv3_data.dist_base)
#define gicv3_redist_base()		(gicv3_data.redist_base[smp_processor_id()])
#define gicv3_sgi_base()		(gicv3_data.redist_base[smp_processor_id()] + SZ_64K)

extern int gicv3_init(void);
extern void gicv3_enable_defaults(void);
extern u32 gicv3_read_iar(void);
extern u32 gicv3_iar_irqnr(u32 iar);
extern void gicv3_write_eoir(u32 irqstat);
extern void gicv3_ipi_send_single(int irq, int cpu);
extern void gicv3_ipi_send_mask(int irq, const cpumask_t *dest);
extern void gicv3_set_redist_base(size_t stride);

static inline void gicv3_do_wait_for_rwp(void *base)
{
	int count = 100000;	/* 1s */

	while (readl(base + GICD_CTLR) & GICD_CTLR_RWP) {
		if (!--count) {
			printf("GICv3: RWP timeout!\n");
			abort();
		}
		cpu_relax();
		udelay(10);
	};
}

static inline void gicv3_dist_wait_for_rwp(void)
{
	gicv3_do_wait_for_rwp(gicv3_dist_base());
}

static inline void gicv3_redist_wait_for_uwp(void)
{
	/*
	 * We can build on gic_do_wait_for_rwp, which uses GICD_ registers
	 * because GICD_CTLR == GICR_CTLR and GICD_CTLR_RWP == GICR_CTLR_UWP
	 */
	gicv3_do_wait_for_rwp(gicv3_redist_base());
}

static inline u32 mpidr_compress(u64 mpidr)
{
	u64 compressed = mpidr & MPIDR_HWID_BITMASK;

	compressed = (((compressed >> 32) & 0xff) << 24) | compressed;
	return compressed;
}

static inline u64 mpidr_uncompress(u32 compressed)
{
	u64 mpidr = ((u64)compressed >> 24) << 32;

	mpidr |= compressed & MPIDR_HWID_BITMASK;
	return mpidr;
}

#endif /* !__ASSEMBLY__ */
#endif /* _ASMARM_GIC_V3_H_ */
