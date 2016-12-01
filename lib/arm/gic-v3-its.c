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

void its_init(void)
{
	if (!its_data.base)
		return;

	its_parse_typer();
}

