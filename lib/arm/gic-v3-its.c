/*
 * Copyright (C) 2016, Red Hat Inc, Eric Auger <eric.auger@redhat.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2.
 */
#include <asm/gic.h>
#include <alloc_page.h>

struct its_data its_data;

void its_parse_typer(void)
{
	u64 typer = readq(gicv3_its_base() + GITS_TYPER);

	its_data.typer.ite_size = ((typer & GITS_TYPER_ITT_ENTRY_SIZE) >>
					GITS_TYPER_ITT_ENTRY_SIZE_SHIFT) + 1;
	its_data.typer.pta = typer & GITS_TYPER_PTA;
	its_data.typer.eventid_bits = ((typer & GITS_TYPER_IDBITS) >>
						GITS_TYPER_IDBITS_SHIFT) + 1;
	its_data.typer.deviceid_bits = ((typer & GITS_TYPER_DEVBITS) >>
						GITS_TYPER_DEVBITS_SHIFT) + 1;

	if (typer & GITS_TYPER_CIL)
		its_data.typer.collid_bits = ((typer & GITS_TYPER_CIDBITS) >>
						GITS_TYPER_CIDBITS_SHIFT) + 1;
	else
		its_data.typer.collid_bits = 16;

	its_data.typer.virt_lpi = typer & GITS_TYPER_VLPIS;
	its_data.typer.phys_lpi = typer & GITS_TYPER_PLPIS;
}

int its_parse_baser(int i, struct its_baser *baser)
{
	void *reg_addr = gicv3_its_base() + GITS_BASER + i * 8;
	u64 val = readq(reg_addr);

	if (!val) {
		memset(baser, 0, sizeof(*baser));
		return -1;
	}

	baser->valid = val & GITS_BASER_VALID;
	baser->indirect = val & GITS_BASER_INDIRECT;
	baser->type = GITS_BASER_TYPE(val);
	baser->esz = GITS_BASER_ENTRY_SIZE(val);
	baser->nr_pages = GITS_BASER_NR_PAGES(val);
	baser->table_addr = val & GITS_BASER_PHYS_ADDR_MASK;
	switch (val & GITS_BASER_PAGE_SIZE_MASK) {
	case GITS_BASER_PAGE_SIZE_4K:
		baser->psz = SZ_4K;
		break;
	case GITS_BASER_PAGE_SIZE_16K:
		baser->psz = SZ_16K;
		break;
	case GITS_BASER_PAGE_SIZE_64K:
		baser->psz = SZ_64K;
		break;
	default:
		baser->psz = SZ_64K;
	}
	return 0;
}

struct its_baser *its_lookup_baser(int type)
{
	int i;

	for (i = 0; i < GITS_BASER_NR_REGS; i++) {
		struct its_baser *baser = &its_data.baser[i];

		if (baser->type == type)
			return baser;
	}
	return NULL;
}

void its_init(void)
{
	int i;

	if (!its_data.base)
		return;

	its_parse_typer();
	for (i = 0; i < GITS_BASER_NR_REGS; i++)
		its_parse_baser(i, &its_data.baser[i]);
}

void its_setup_baser(int i, struct its_baser *baser)
{
	unsigned long n = (baser->nr_pages * baser->psz) >> PAGE_SHIFT;
	unsigned long order = is_power_of_2(n) ? fls(n) : fls(n) + 1;
	u64 val;

	baser->table_addr = (u64)virt_to_phys(alloc_pages(order));

	val = ((u64)baser->table_addr					|
		((u64)baser->type	<< GITS_BASER_TYPE_SHIFT)	|
		((u64)(baser->esz - 1)	<< GITS_BASER_ENTRY_SIZE_SHIFT)	|
		((baser->nr_pages - 1)	<< GITS_BASER_PAGES_SHIFT)	|
		(u64)baser->indirect	<< 62				|
		(u64)baser->valid	<< 63);

	switch (baser->psz) {
	case SZ_4K:
		val |= GITS_BASER_PAGE_SIZE_4K;
		break;
	case SZ_16K:
		val |= GITS_BASER_PAGE_SIZE_16K;
		break;
	case SZ_64K:
		val |= GITS_BASER_PAGE_SIZE_64K;
		break;
	}

	writeq(val, gicv3_its_base() + GITS_BASER + i * 8);
}

