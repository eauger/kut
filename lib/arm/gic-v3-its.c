/*
 * Copyright (C) 2016, Red Hat Inc, Eric Auger <eric.auger@redhat.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2.
 */
#include <asm/gic.h>

struct its_data its_data;

void its_parse_typer(void)
{
	u64 typer = readq(gicv3_its_base() + GITS_TYPER);

	its_data.typer.ite_size = ((typer >> 4) & 0xf) + 1;
	its_data.typer.pta = typer & GITS_TYPER_PTA;
	its_data.typer.eventid_bits =
		((typer >> GITS_TYPER_IDBITS_SHIFT) & 0x1f) + 1;
	its_data.typer.deviceid_bits = GITS_TYPER_DEVBITS(typer) + 1;

	its_data.typer.cil = (typer >> 36) & 0x1;
	if (its_data.typer.cil)
		its_data.typer.collid_bits = ((typer >> 32) & 0xf) + 1;
	else
		its_data.typer.collid_bits = 16;

	its_data.typer.hw_collections =
		(typer >> GITS_TYPER_HWCOLLCNT_SHIFT) & 0xff;

	its_data.typer.cct = typer & 0x4;
	its_data.typer.virt_lpi = typer & 0x2;
	its_data.typer.phys_lpi = typer & GITS_TYPER_PLPIS;
}

void its_init(void)
{
	if (!its_data.base)
		return;

	its_parse_typer();
}

