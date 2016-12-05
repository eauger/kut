/*
 * All ITS* defines are lifted from include/linux/irqchip/arm-gic-v3.h
 *
 * Copyright (C) 2016, Red Hat Inc, Andrew Jones <drjones@redhat.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2.
 */
#ifndef _ASMARM_GIC_V3_ITS_H_
#define _ASMARM_GIC_V3_ITS_H_

#ifndef __ASSEMBLY__

#define GITS_CTLR			0x0000
#define GITS_IIDR			0x0004
#define GITS_TYPER			0x0008
#define GITS_CBASER			0x0080
#define GITS_CWRITER			0x0088
#define GITS_CREADR			0x0090
#define GITS_BASER			0x0100

#define GITS_TYPER_PLPIS                (1UL << 0)
#define GITS_TYPER_IDBITS_SHIFT         8
#define GITS_TYPER_DEVBITS_SHIFT        13
#define GITS_TYPER_DEVBITS(r)           ((((r) >> GITS_TYPER_DEVBITS_SHIFT) & 0x1f) + 1)
#define GITS_TYPER_PTA                  (1UL << 19)
#define GITS_TYPER_HWCOLLCNT_SHIFT      24

#define GITS_CTLR_ENABLE                (1U << 0)

#define GITS_CBASER_VALID                       (1UL << 63)
#define GITS_CBASER_SHAREABILITY_SHIFT          (10)
#define GITS_CBASER_INNER_CACHEABILITY_SHIFT    (59)
#define GITS_CBASER_OUTER_CACHEABILITY_SHIFT    (53)
#define GITS_CBASER_SHAREABILITY_MASK                                   \
	GIC_BASER_SHAREABILITY(GITS_CBASER, SHAREABILITY_MASK)
#define GITS_CBASER_INNER_CACHEABILITY_MASK                             \
	GIC_BASER_CACHEABILITY(GITS_CBASER, INNER, MASK)
#define GITS_CBASER_OUTER_CACHEABILITY_MASK                             \
	GIC_BASER_CACHEABILITY(GITS_CBASER, OUTER, MASK)
#define GITS_CBASER_CACHEABILITY_MASK GITS_CBASER_INNER_CACHEABILITY_MASK

#define GITS_CBASER_InnerShareable                                      \
	GIC_BASER_SHAREABILITY(GITS_CBASER, InnerShareable)

#define GITS_CBASER_nCnB        GIC_BASER_CACHEABILITY(GITS_CBASER, INNER, nCnB)
#define GITS_CBASER_nC          GIC_BASER_CACHEABILITY(GITS_CBASER, INNER, nC)
#define GITS_CBASER_RaWt        GIC_BASER_CACHEABILITY(GITS_CBASER, INNER, RaWt)
#define GITS_CBASER_RaWb        GIC_BASER_CACHEABILITY(GITS_CBASER, INNER, RaWt)
#define GITS_CBASER_WaWt        GIC_BASER_CACHEABILITY(GITS_CBASER, INNER, WaWt)
#define GITS_CBASER_WaWb        GIC_BASER_CACHEABILITY(GITS_CBASER, INNER, WaWb)
#define GITS_CBASER_RaWaWt      GIC_BASER_CACHEABILITY(GITS_CBASER, INNER, RaWaWt)
#define GITS_CBASER_RaWaWb      GIC_BASER_CACHEABILITY(GITS_CBASER, INNER, RaWaWb)

#define GITS_BASER_NR_REGS              8

#define GITS_BASER_VALID                        (1UL << 63)
#define GITS_BASER_INDIRECT                     (1ULL << 62)

#define GITS_BASER_INNER_CACHEABILITY_SHIFT     (59)
#define GITS_BASER_OUTER_CACHEABILITY_SHIFT     (53)
#define GITS_BASER_CACHEABILITY_MASK		0x7

#define GITS_BASER_nCnB         GIC_BASER_CACHEABILITY(GITS_BASER, INNER, nCnB)

#define GITS_BASER_TYPE_SHIFT                   (56)
#define GITS_BASER_TYPE(r)              (((r) >> GITS_BASER_TYPE_SHIFT) & 7)
#define GITS_BASER_ENTRY_SIZE_SHIFT             (48)
#define GITS_BASER_ENTRY_SIZE(r)        ((((r) >> GITS_BASER_ENTRY_SIZE_SHIFT) & 0x1f) + 1)
#define GITS_BASER_SHAREABILITY_SHIFT   (10)
#define GITS_BASER_InnerShareable                                       \
	GIC_BASER_SHAREABILITY(GITS_BASER, InnerShareable)
#define GITS_BASER_PAGE_SIZE_SHIFT      (8)
#define GITS_BASER_PAGE_SIZE_4K         (0UL << GITS_BASER_PAGE_SIZE_SHIFT)
#define GITS_BASER_PAGE_SIZE_16K        (1UL << GITS_BASER_PAGE_SIZE_SHIFT)
#define GITS_BASER_PAGE_SIZE_64K        (2UL << GITS_BASER_PAGE_SIZE_SHIFT)
#define GITS_BASER_PAGE_SIZE_MASK       (3UL << GITS_BASER_PAGE_SIZE_SHIFT)
#define GITS_BASER_PAGES_MAX            256
#define GITS_BASER_PAGES_SHIFT          (0)
#define GITS_BASER_NR_PAGES(r)          (((r) & 0xff) + 1)
#define GITS_BASER_PHYS_ADDR_MASK	0xFFFFFFFFF000

#define GITS_BASER_TYPE_NONE            0
#define GITS_BASER_TYPE_DEVICE          1
#define GITS_BASER_TYPE_VCPU            2
#define GITS_BASER_TYPE_CPU             3
#define GITS_BASER_TYPE_COLLECTION      4

#define ITS_FLAGS_CMDQ_NEEDS_FLUSHING           (1ULL << 0)

#define GITS_MAX_DEVICES		8
#define GITS_MAX_COLLECTIONS		8

/*
 * ITS commands
 */
#define GITS_CMD_MAPD                   0x08
#define GITS_CMD_MAPC                   0x09
#define GITS_CMD_MAPTI                  0x0a
/* older GIC documentation used MAPVI for this command */
#define GITS_CMD_MAPVI                  GITS_CMD_MAPTI
#define GITS_CMD_MAPI                   0x0b
#define GITS_CMD_MOVI                   0x01
#define GITS_CMD_DISCARD                0x0f
#define GITS_CMD_INV                    0x0c
#define GITS_CMD_MOVALL                 0x0e
#define GITS_CMD_INVALL                 0x0d
#define GITS_CMD_INT                    0x03
#define GITS_CMD_CLEAR                  0x04
#define GITS_CMD_SYNC                   0x05

struct its_typer {
	unsigned int ite_size;
	unsigned int eventid_bits;
	unsigned int deviceid_bits;
	unsigned int collid_bits;
	unsigned int hw_collections;
	bool pta;
	bool cil;
	bool cct;
	bool phys_lpi;
	bool virt_lpi;
};

struct its_baser {
	unsigned int index;
	int type;
	u64 cache;
	int shr;
	size_t psz;
	int nr_pages;
	bool indirect;
	phys_addr_t table_addr;
	bool valid;
	int esz;
};

struct its_cmd_block {
	u64     raw_cmd[4];
};

struct its_device {
	u32 device_id;	/* device ID */
	u32 nr_ites;	/* Max Interrupt Translation Entries */
	void *itt;	/* Interrupt Translation Table GPA */
};

struct its_collection {
	u64 target_address;
	u16 col_id;
};

struct its_data {
	void *base;
	struct its_typer typer;
	struct its_baser baser[GITS_BASER_NR_REGS];
	struct its_cmd_block *cmd_base;
	struct its_cmd_block *cmd_write;
	struct its_cmd_block *cmd_readr;
	struct its_device devices[GITS_MAX_DEVICES];
	u32 nb_devices;		/* Allocated Devices */
	struct its_collection collections[GITS_MAX_COLLECTIONS];
	u32 nb_collections;	/* Allocated Collections */
};

struct its_event {
	int cpu_id;
	int lpi_id;
};

struct its_stats {
	struct its_event expected;
	struct its_event observed;
};

extern struct its_data its_data;

#define gicv3_its_base()		(its_data.base)

extern void its_parse_typer(void);
extern void its_init(void);
extern int its_parse_baser(int i, struct its_baser *baser);
extern void its_setup_baser(int i, struct its_baser *baser);
extern struct its_baser *its_lookup_baser(int type);
extern void set_lpi_config(int n, u8 val);
extern u8 get_lpi_config(int n);
extern void set_pending_table_bit(int rdist, int n, bool set);
extern void gicv3_rdist_ctrl_lpi(u32 redist, bool set);
extern void its_enable_defaults(void);
extern struct its_device *its_create_device(u32 dev_id, int nr_ites);
extern struct its_collection *its_create_collection(u32 col_id, u32 target_pe);
extern struct its_collection *its_create_collection(u32 col_id, u32 target);

extern void set_lpi_config(int n, u8 val);
extern u8 get_lpi_config(int n);

extern void its_send_mapd(struct its_device *dev, int valid);
extern void its_send_mapc(struct its_collection *col, int valid);
extern void its_send_mapti(struct its_device *dev, u32 irq_id,
			   u32 event_id, struct its_collection *col);
extern void its_send_int(struct its_device *dev, u32 event_id);
extern void its_send_inv(struct its_device *dev, u32 event_id);
extern void its_send_discard(struct its_device *dev, u32 event_id);
extern void its_send_clear(struct its_device *dev, u32 event_id);
extern void its_send_invall(struct its_collection *col);
extern void its_send_movi(struct its_device *dev,
			  struct its_collection *col, u32 id);
extern void its_send_sync(struct its_collection *col);
extern void its_print_cmd_state(void);

#define ITS_FLAGS_CMDQ_NEEDS_FLUSHING           (1ULL << 0)
#define ITS_FLAGS_WORKAROUND_CAVIUM_22375       (1ULL << 1)
#define ITS_FLAGS_WORKAROUND_CAVIUM_23144       (1ULL << 2)

#endif /* !__ASSEMBLY__ */
#endif /* _ASMARM_GIC_V3_ITS_H_ */
