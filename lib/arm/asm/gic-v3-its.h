/*
 * ITS 32-bit stubs
 *
 * Copyright (C) 2020, Red Hat Inc, Eric Auger <eric.auger@redhat.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2.
 */
#ifndef _ASMARM_GIC_V3_ITS_H_
#define _ASMARM_GIC_V3_ITS_H_

/* dummy its_data struct to allow gic_get_dt_bases() call */
struct its_data {
	void *base;
};

static inline void its_init(void) {}
static inline void test_its_introspection(void)
{
	report_abort("not supported on 32-bit");
}
static inline void test_its_trigger(void)
{
	report_abort("not supported on 32-bit");
}
static inline void test_its_migration(void)
{
	report_abort("not supported on 32-bit");
}

#endif /* _ASMARM_GICv3_ITS */
