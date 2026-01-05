// SPDX-License-Identifier: GPL-2.0
/*
 * VMEM Manager hotplug scaffold.
 */

#include <linux/kernel.h>

#include "vmem_mgr.h"

int vmem_mgr_hotplug_init(struct vmem_mgr *mgr)
{
	(void)mgr;
	return 0;
}

void vmem_mgr_hotplug_exit(struct vmem_mgr *mgr)
{
	(void)mgr;
}
