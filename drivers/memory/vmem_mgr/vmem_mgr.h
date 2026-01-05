/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_VMEM_MGR_H
#define _LINUX_VMEM_MGR_H

#include <linux/mutex.h>

#define VMEM_MGR_BLOCKS            128
#define VMEM_MGR_BLOCK_SIZE_MB     128
#define VMEM_MGR_FIXED_B_BLOCKS    8

enum vmem_view_mode {
	VMEM_VIEW_4CH = 0,
	VMEM_VIEW_2CH,
};

enum vmem_block_state {
	VMEM_BLOCK_ACTIVE_A = 0,
	VMEM_BLOCK_MIGRATING,
	VMEM_BLOCK_ACTIVE_B,
	VMEM_BLOCK_FIXED_B,
};

struct vmem_block {
	unsigned int id;
	unsigned long phys_addr;
	enum vmem_view_mode current_view;
	enum vmem_block_state state;
	struct mutex lock;
};

struct vmem_mgr {
	struct mutex lock;
	struct vmem_block blocks[VMEM_MGR_BLOCKS];
	unsigned long view_a_base;
	unsigned long view_b_base;
	int active_phy_mask;
	enum vmem_view_mode mode;
};

int vmem_mgr_numa_init(struct vmem_mgr *mgr);
void vmem_mgr_numa_exit(struct vmem_mgr *mgr);

int vmem_mgr_hotplug_init(struct vmem_mgr *mgr);
void vmem_mgr_hotplug_exit(struct vmem_mgr *mgr);

int vmem_mgr_reorder_init(struct vmem_mgr *mgr);
void vmem_mgr_reorder_exit(struct vmem_mgr *mgr);

#endif /* _LINUX_VMEM_MGR_H */
