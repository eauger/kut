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

inline void set_lpi_config(int n, u8 value)
{
	u8 *entry = (u8 *)(gicv3_data.lpi_prop + (n - 8192));
	*entry = value;
}

inline u8 get_lpi_config(int n)
{
	u8 *entry = (u8 *)(gicv3_data.lpi_prop + (n - 8192));
	return *entry;
}

/* alloc_lpi_tables: Allocate LPI config and pending tables */
void alloc_lpi_tables(void);
void alloc_lpi_tables(void)
{
	unsigned long n = SZ_64K >> PAGE_SHIFT;
	unsigned long order = fls(n);
	u64 prop_val;
	int cpu;

	gicv3_data.lpi_prop = (void *)virt_to_phys(alloc_pages(order));

	/* ID bits = 13, ie. up to 14b LPI INTID */
	prop_val = (u64)gicv3_data.lpi_prop | 13;

	/*
	 * Allocate pending tables for each redistributor
	 * and set PROPBASER and PENDBASER
	 */
	for_each_present_cpu(cpu) {
		u64 pend_val;
		void *ptr;

		ptr = gicv3_data.redist_base[cpu];

		writeq(prop_val, ptr + GICR_PROPBASER);

		gicv3_data.lpi_pend[cpu] =
			(void *)virt_to_phys(alloc_pages(order));

		pend_val = (u64)gicv3_data.lpi_pend[cpu];

		writeq(pend_val, ptr + GICR_PENDBASER);
	}
}

void set_pending_table_bit(int rdist, int n, bool set)
{
	u8 *ptr = phys_to_virt((phys_addr_t)gicv3_data.lpi_pend[rdist]);
	u8 mask = 1 << (n % 8), byte;

	ptr += (n / 8);
	byte = *ptr;
	if (set)
		byte |=  mask;
	else
		byte &= ~mask;
	*ptr = byte;
}

/**
 * init_cmd_queue: Allocate the command queue and initialize
 * CBASER, CREADR, CWRITER
 */
void init_cmd_queue(void);
void init_cmd_queue(void)
{
	unsigned long n = SZ_64K >> PAGE_SHIFT;
	unsigned long order = fls(n);
	u64 cbaser;

	its_data.cmd_base = (void *)virt_to_phys(alloc_pages(order));

	cbaser = ((u64)its_data.cmd_base | (SZ_64K / SZ_4K - 1)	|
			GITS_CBASER_VALID);

	writeq(cbaser, its_data.base + GITS_CBASER);

	its_data.cmd_write = its_data.cmd_base;
	writeq(0, its_data.base + GITS_CWRITER);
}

void gicv3_rdist_ctrl_lpi(u32 redist, bool set)
{
	void *ptr;
	u64 val;

	if (redist >= nr_cpus)
		report_abort("%s redist=%d >= cpu_count=%d\n",
			     __func__, redist, nr_cpus);

	ptr = gicv3_data.redist_base[redist];
	val = readl(ptr + GICR_CTLR);
	if (set)
		val |= GICR_CTLR_ENABLE_LPIS;
	else
		val &= ~GICR_CTLR_ENABLE_LPIS;
	writel(val,  ptr + GICR_CTLR);
}

void its_enable_defaults(void)
{
	unsigned int i;

	its_parse_typer();

	/* Allocate BASER tables (device and collection tables) */
	for (i = 0; i < GITS_BASER_NR_REGS; i++) {
		struct its_baser *baser = &its_data.baser[i];
		int ret;

		ret = its_parse_baser(i, baser);
		if (ret)
			continue;

		switch (baser->type) {
		case GITS_BASER_TYPE_DEVICE:
			baser->valid = true;
			its_setup_baser(i, baser);
			break;
		case GITS_BASER_TYPE_COLLECTION:
			baser->valid = true;
			its_setup_baser(i, baser);
			break;
		default:
			break;
		}
	}

	/* Allocate LPI config and pending tables */
	alloc_lpi_tables();

	init_cmd_queue();

	for (i = 0; i < nr_cpus; i++)
		gicv3_rdist_ctrl_lpi(i, true);

	writel(GITS_CTLR_ENABLE, its_data.base + GITS_CTLR);
}

struct its_device *its_create_device(u32 device_id, int nr_ites)
{
	struct its_baser *baser;
	struct its_device *new;
	unsigned long n, order;

	if (its_data.nr_devices >= GITS_MAX_DEVICES)
		report_abort("%s redimension GITS_MAX_DEVICES", __func__);

	baser = its_lookup_baser(GITS_BASER_TYPE_DEVICE);
	if (!baser)
		return NULL;

	new = &its_data.devices[its_data.nr_devices];

	new->device_id = device_id;
	new->nr_ites = nr_ites;

	n = (its_data.typer.ite_size * nr_ites) >> PAGE_SHIFT;
	order = is_power_of_2(n) ? fls(n) : fls(n) + 1;
	new->itt = (void *)virt_to_phys(alloc_pages(order));

	its_data.nr_devices++;
	return new;
}

struct its_collection *its_create_collection(u32 col_id, u32 pe)
{
	struct its_collection *new;

	if (its_data.nr_collections >= GITS_MAX_COLLECTIONS)
		report_abort("%s redimension GITS_MAX_COLLECTIONS", __func__);

	new = &its_data.collections[its_data.nr_collections];

	new->col_id = col_id;

	if (its_data.typer.pta)
		new->target_address = (u64)gicv3_data.redist_base[pe];
	else
		new->target_address = pe << 16;

	its_data.nr_collections++;
	return new;
}
