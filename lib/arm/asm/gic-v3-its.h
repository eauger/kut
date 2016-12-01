/*
 * ITS 32-bit stubs
 *
 * Copyright (C) 2020, Red Hat Inc, Eric Auger <eric.auger@redhat.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2.
 */

#ifndef _ASMARM_GICv3_ITS
#define _ASMARM_GICv3_ITS

/* dummy its_data struct to allow gic_get_dt_bases() call */
struct its_data {
	void *base;
};

static inline void its_init(void) {}
static inline void test_its_introspection(void)
{
	report_abort("not supported on 32-bit");
}

#endif /* _ASMARM_GICv3_ITS */
