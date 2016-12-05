/*
 * Copyright (C) 2016, Red Hat Inc, Eric Auger <eric.auger@redhat.com>
 *
 * Most of the code is copy-pasted from:
 * drivers/irqchip/irq-gic-v3-its.c
 * This work is licensed under the terms of the GNU LGPL, version 2.
 */
#include <asm/io.h>
#include <asm/gic.h>

#define ITS_ITT_ALIGN           SZ_256

static const char * const its_cmd_string[] = {
	[GITS_CMD_MAPD]		= "MAPD",
	[GITS_CMD_MAPC]		= "MAPC",
	[GITS_CMD_MAPTI]	= "MAPTI",
	[GITS_CMD_MAPI]		= "MAPI",
	[GITS_CMD_MOVI]		= "MOVI",
	[GITS_CMD_DISCARD]	= "DISCARD",
	[GITS_CMD_INV]		= "INV",
	[GITS_CMD_MOVALL]	= "MOVALL",
	[GITS_CMD_INVALL]	= "INVALL",
	[GITS_CMD_INT]		= "INT",
	[GITS_CMD_CLEAR]	= "CLEAR",
	[GITS_CMD_SYNC]		= "SYNC",
};

struct its_cmd_desc {
	union {
		struct {
			struct its_device *dev;
			u32 event_id;
		} its_inv_cmd;

		struct {
			struct its_device *dev;
			u32 event_id;
		} its_int_cmd;

		struct {
			struct its_device *dev;
			bool valid;
		} its_mapd_cmd;

		struct {
			struct its_collection *col;
			bool valid;
		} its_mapc_cmd;

		struct {
			struct its_device *dev;
			u32 phys_id;
			u32 event_id;
			u32 col_id;
		} its_mapti_cmd;

		struct {
			struct its_device *dev;
			struct its_collection *col;
			u32 event_id;
		} its_movi_cmd;

		struct {
			struct its_device *dev;
			u32 event_id;
		} its_discard_cmd;

		struct {
			struct its_device *dev;
			u32 event_id;
		} its_clear_cmd;

		struct {
			struct its_collection *col;
		} its_invall_cmd;

		struct {
			struct its_collection *col;
		} its_sync_cmd;
	};
};

typedef void (*its_cmd_builder_t)(struct its_cmd_block *,
				  struct its_cmd_desc *);

/* ITS COMMANDS */

static void its_encode_cmd(struct its_cmd_block *cmd, u8 cmd_nr)
{
	cmd->raw_cmd[0] &= ~0xffUL;
	cmd->raw_cmd[0] |= cmd_nr;
}

static void its_encode_devid(struct its_cmd_block *cmd, u32 devid)
{
	cmd->raw_cmd[0] &= BIT_ULL(32) - 1;
	cmd->raw_cmd[0] |= ((u64)devid) << 32;
}

static void its_encode_event_id(struct its_cmd_block *cmd, u32 id)
{
	cmd->raw_cmd[1] &= ~0xffffffffUL;
	cmd->raw_cmd[1] |= id;
}

static void its_encode_phys_id(struct its_cmd_block *cmd, u32 phys_id)
{
	cmd->raw_cmd[1] &= 0xffffffffUL;
	cmd->raw_cmd[1] |= ((u64)phys_id) << 32;
}

static void its_encode_size(struct its_cmd_block *cmd, u8 size)
{
	cmd->raw_cmd[1] &= ~0x1fUL;
	cmd->raw_cmd[1] |= size & 0x1f;
}

static void its_encode_itt(struct its_cmd_block *cmd, u64 itt_addr)
{
	cmd->raw_cmd[2] &= ~0xffffffffffffUL;
	cmd->raw_cmd[2] |= itt_addr & 0xffffffffff00UL;
}

static void its_encode_valid(struct its_cmd_block *cmd, int valid)
{
	cmd->raw_cmd[2] &= ~(1UL << 63);
	cmd->raw_cmd[2] |= ((u64)!!valid) << 63;
}

static void its_encode_target(struct its_cmd_block *cmd, u64 target_addr)
{
	cmd->raw_cmd[2] &= ~(0xfffffffffUL << 16);
	cmd->raw_cmd[2] |= (target_addr & (0xffffffffUL << 16));
}

static void its_encode_collection(struct its_cmd_block *cmd, u16 col)
{
	cmd->raw_cmd[2] &= ~0xffffUL;
	cmd->raw_cmd[2] |= col;
}

static inline void its_fixup_cmd(struct its_cmd_block *cmd)
{
	/* Let's fixup BE commands */
	cmd->raw_cmd[0] = cpu_to_le64(cmd->raw_cmd[0]);
	cmd->raw_cmd[1] = cpu_to_le64(cmd->raw_cmd[1]);
	cmd->raw_cmd[2] = cpu_to_le64(cmd->raw_cmd[2]);
	cmd->raw_cmd[3] = cpu_to_le64(cmd->raw_cmd[3]);
}

static u64 its_cmd_ptr_to_offset(struct its_cmd_block *ptr)
{
	return (ptr - its_data.cmd_base) * sizeof(*ptr);
}

static struct its_cmd_block *its_post_commands(void)
{
	u64 wr = its_cmd_ptr_to_offset(its_data.cmd_write);

	writeq(wr, its_data.base + GITS_CWRITER);
	return its_data.cmd_write;
}


/* We just assume the queue is large enough */
static struct its_cmd_block *its_allocate_entry(void)
{
	struct its_cmd_block *cmd;

	cmd = its_data.cmd_write++;
	return cmd;
}

static void its_wait_for_range_completion(struct its_cmd_block *from,
					  struct its_cmd_block *to)
{
	u64 rd_idx, from_idx, to_idx;
	u32 count = 1000000;    /* 1s! */

	from_idx = its_cmd_ptr_to_offset(from);
	to_idx = its_cmd_ptr_to_offset(to);
	while (1) {
		rd_idx = readq(its_data.base + GITS_CREADR);
		if (rd_idx >= to_idx || rd_idx < from_idx)
			break;

		count--;
		if (!count) {
			unsigned int cmd_id = from->raw_cmd[0] & 0xFF;

			report(false, "%s timeout!",
			       cmd_id <= 0xF ? its_cmd_string[cmd_id] :
			       "Unexpected");
			return;
		}
		cpu_relax();
		udelay(1);
	}
}

void its_print_cmd_state(void)
{
	u64 rd, wr;

	rd = readq(its_data.base + GITS_CREADR);
	wr = readq(its_data.base + GITS_CWRITER);
	report_info("GITS_CREADR=0x%lx GITS_CWRITER=0x%lx", rd, wr);
}

static void its_send_single_command(its_cmd_builder_t builder,
				    struct its_cmd_desc *desc)
{
	struct its_cmd_block *cmd, *next_cmd;

	cmd = its_allocate_entry();
	builder(cmd, desc);
	next_cmd = its_post_commands();

	its_wait_for_range_completion(cmd, next_cmd);
}


static void its_build_mapd_cmd(struct its_cmd_block *cmd,
			       struct its_cmd_desc *desc)
{
	unsigned long itt_addr;
	u8 size = 12; //TODO ilog2(desc->its_mapd_cmd.dev->nr_ites);

	itt_addr = (unsigned long)desc->its_mapd_cmd.dev->itt;
	itt_addr = ALIGN(itt_addr, ITS_ITT_ALIGN);

	its_encode_cmd(cmd, GITS_CMD_MAPD);
	its_encode_devid(cmd, desc->its_mapd_cmd.dev->device_id);
	its_encode_size(cmd, size - 1);
	its_encode_itt(cmd, itt_addr);
	its_encode_valid(cmd, desc->its_mapd_cmd.valid);

	its_fixup_cmd(cmd);
	report_info("MAPD devid=%d size = 0x%x itt=0x%lx valid=%d",
		    desc->its_mapd_cmd.dev->device_id,
		    size, itt_addr, desc->its_mapd_cmd.valid);

}

static void its_build_mapc_cmd(struct its_cmd_block *cmd,
			       struct its_cmd_desc *desc)
{
	its_encode_cmd(cmd, GITS_CMD_MAPC);
	its_encode_collection(cmd, desc->its_mapc_cmd.col->col_id);
	its_encode_target(cmd, desc->its_mapc_cmd.col->target_address);
	its_encode_valid(cmd, desc->its_mapc_cmd.valid);

	its_fixup_cmd(cmd);
	report_info("MAPC col_id=%d target_addr = 0x%lx valid=%d",
		    desc->its_mapc_cmd.col->col_id,
		    desc->its_mapc_cmd.col->target_address,
		    desc->its_mapc_cmd.valid);
}

static void its_build_mapti_cmd(struct its_cmd_block *cmd,
				struct its_cmd_desc *desc)
{
	its_encode_cmd(cmd, GITS_CMD_MAPTI);
	its_encode_devid(cmd, desc->its_mapti_cmd.dev->device_id);
	its_encode_event_id(cmd, desc->its_mapti_cmd.event_id);
	its_encode_phys_id(cmd, desc->its_mapti_cmd.phys_id);
	its_encode_collection(cmd, desc->its_mapti_cmd.col_id);

	its_fixup_cmd(cmd);
	report_info("MAPTI dev_id=%d event_id=%d -> phys_id=%d, col_id=%d",
		    desc->its_mapti_cmd.dev->device_id,
		    desc->its_mapti_cmd.event_id,
		    desc->its_mapti_cmd.phys_id,
		    desc->its_mapti_cmd.col_id);
}

static void its_build_invall_cmd(struct its_cmd_block *cmd,
			      struct its_cmd_desc *desc)
{
	its_encode_cmd(cmd, GITS_CMD_INVALL);
	its_encode_collection(cmd, desc->its_invall_cmd.col->col_id);

	its_fixup_cmd(cmd);
	report_info("INVALL col_id=%d", desc->its_invall_cmd.col->col_id);
}

static void its_build_clear_cmd(struct its_cmd_block *cmd,
				struct its_cmd_desc *desc)
{
	its_encode_cmd(cmd, GITS_CMD_CLEAR);
	its_encode_devid(cmd, desc->its_clear_cmd.dev->device_id);
	its_encode_event_id(cmd, desc->its_clear_cmd.event_id);

	its_fixup_cmd(cmd);
	report_info("CLEAR col_id=%d", desc->its_invall_cmd.col->col_id);
}

static void its_build_discard_cmd(struct its_cmd_block *cmd,
				  struct its_cmd_desc *desc)
{
	its_encode_cmd(cmd, GITS_CMD_DISCARD);
	its_encode_devid(cmd, desc->its_discard_cmd.dev->device_id);
	its_encode_event_id(cmd, desc->its_discard_cmd.event_id);

	its_fixup_cmd(cmd);
	report_info("DISCARD col_id=%d", desc->its_invall_cmd.col->col_id);
}

static void its_build_inv_cmd(struct its_cmd_block *cmd,
			      struct its_cmd_desc *desc)
{
	its_encode_cmd(cmd, GITS_CMD_INV);
	its_encode_devid(cmd, desc->its_inv_cmd.dev->device_id);
	its_encode_event_id(cmd, desc->its_inv_cmd.event_id);

	its_fixup_cmd(cmd);
	report_info("INV dev_id=%d event_id=%d",
		    desc->its_inv_cmd.dev->device_id,
		    desc->its_inv_cmd.event_id);
}

static void its_build_int_cmd(struct its_cmd_block *cmd,
			      struct its_cmd_desc *desc)
{
	its_encode_cmd(cmd, GITS_CMD_INT);
	its_encode_devid(cmd, desc->its_int_cmd.dev->device_id);
	its_encode_event_id(cmd, desc->its_int_cmd.event_id);

	its_fixup_cmd(cmd);
	report_info("INT dev_id=%d event_id=%d",
		    desc->its_int_cmd.dev->device_id,
		    desc->its_int_cmd.event_id);
}

static void its_build_sync_cmd(struct its_cmd_block *cmd,
			       struct its_cmd_desc *desc)
{
	its_encode_cmd(cmd, GITS_CMD_SYNC);
	its_encode_target(cmd, desc->its_sync_cmd.col->target_address);
	its_fixup_cmd(cmd);
	report_info("SYNC target_addr = 0x%lx",
		    desc->its_sync_cmd.col->target_address);
}

static void its_build_movi_cmd(struct its_cmd_block *cmd,
			       struct its_cmd_desc *desc)
{
	its_encode_cmd(cmd, GITS_CMD_MOVI);
	its_encode_devid(cmd, desc->its_movi_cmd.dev->device_id);
	its_encode_event_id(cmd, desc->its_movi_cmd.event_id);
	its_encode_collection(cmd, desc->its_movi_cmd.col->col_id);

	its_fixup_cmd(cmd);
	report_info("MOVI dev_id=%d event_id = %d col_id=%d",
		    desc->its_movi_cmd.dev->device_id,
		    desc->its_movi_cmd.event_id,
		    desc->its_movi_cmd.col->col_id);
}

void its_send_mapd(struct its_device *dev, int valid)
{
	struct its_cmd_desc desc;

	desc.its_mapd_cmd.dev = dev;
	desc.its_mapd_cmd.valid = !!valid;

	its_send_single_command(its_build_mapd_cmd, &desc);
}

void its_send_mapc(struct its_collection *col, int valid)
{
	struct its_cmd_desc desc;

	desc.its_mapc_cmd.col = col;
	desc.its_mapc_cmd.valid = !!valid;

	its_send_single_command(its_build_mapc_cmd, &desc);
}

void its_send_mapti(struct its_device *dev, u32 irq_id,
		    u32 event_id, struct its_collection *col)
{
	struct its_cmd_desc desc;

	desc.its_mapti_cmd.dev = dev;
	desc.its_mapti_cmd.phys_id = irq_id;
	desc.its_mapti_cmd.event_id = event_id;
	desc.its_mapti_cmd.col_id = col->col_id;

	its_send_single_command(its_build_mapti_cmd, &desc);
}

void its_send_int(struct its_device *dev, u32 event_id)
{
	struct its_cmd_desc desc;

	desc.its_int_cmd.dev = dev;
	desc.its_int_cmd.event_id = event_id;

	its_send_single_command(its_build_int_cmd, &desc);
}

void its_send_movi(struct its_device *dev,
		   struct its_collection *col, u32 id)
{
	struct its_cmd_desc desc;

	desc.its_movi_cmd.dev = dev;
	desc.its_movi_cmd.col = col;
	desc.its_movi_cmd.event_id = id;

	its_send_single_command(its_build_movi_cmd, &desc);
}

void its_send_invall(struct its_collection *col)
{
	struct its_cmd_desc desc;

	desc.its_invall_cmd.col = col;

	its_send_single_command(its_build_invall_cmd, &desc);
}

void its_send_inv(struct its_device *dev, u32 event_id)
{
	struct its_cmd_desc desc;

	desc.its_inv_cmd.dev = dev;
	desc.its_inv_cmd.event_id = event_id;

	its_send_single_command(its_build_inv_cmd, &desc);
}

void its_send_discard(struct its_device *dev, u32 event_id)
{
	struct its_cmd_desc desc;

	desc.its_discard_cmd.dev = dev;
	desc.its_discard_cmd.event_id = event_id;

	its_send_single_command(its_build_discard_cmd, &desc);
}

void its_send_clear(struct its_device *dev, u32 event_id)
{
	struct its_cmd_desc desc;

	desc.its_clear_cmd.dev = dev;
	desc.its_clear_cmd.event_id = event_id;

	its_send_single_command(its_build_clear_cmd, &desc);
}

void its_send_sync(struct its_collection *col)
{
	struct its_cmd_desc desc;

	desc.its_sync_cmd.col = col;

	its_send_single_command(its_build_sync_cmd, &desc);
}

