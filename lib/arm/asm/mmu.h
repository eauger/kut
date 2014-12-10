#ifndef __ASMARM_MMU_H_
#define __ASMARM_MMU_H_
/*
 * Copyright (C) 2014, Red Hat Inc, Andrew Jones <drjones@redhat.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2.
 */
#include <asm/pgtable.h>
#include <asm/barrier.h>

static inline void local_flush_tlb_all(void)
{
	asm volatile("mcr p15, 0, %0, c8, c7, 0" :: "r" (0));
	dsb();
	isb();
}

static inline void flush_tlb_all(void)
{
	//TODO
	local_flush_tlb_all();
}

extern bool mmu_enabled(void);
extern void mmu_enable(pgd_t *pgtable);
extern void mmu_enable_idmap(void);
extern void mmu_init_io_sect(pgd_t *pgtable);

#endif /* __ASMARM_MMU_H_ */
