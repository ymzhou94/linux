// SPDX-License-Identifier: GPL-2.0
/*
 * VMEM Manager core scaffold for LPDDR6 V-Topology.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#include "vmem_mgr.h"

static struct vmem_mgr vmem_mgr;
static struct kobject *vmem_kobj;

static const char *vmem_view_name(enum vmem_view_mode mode)
{
	switch (mode) {
	case VMEM_VIEW_4CH:
		return "view_a";
	case VMEM_VIEW_2CH:
		return "view_b";
	default:
		return "unknown";
	}
}

static const char *vmem_block_state_name(enum vmem_block_state state)
{
	switch (state) {
	case VMEM_BLOCK_ACTIVE_A:
		return "active_a";
	case VMEM_BLOCK_MIGRATING:
		return "migrating";
	case VMEM_BLOCK_ACTIVE_B:
		return "active_b";
	case VMEM_BLOCK_FIXED_B:
		return "fixed_b";
	default:
		return "unknown";
	}
}

static void vmem_mgr_update_blocks(struct vmem_mgr *mgr, enum vmem_view_mode mode)
{
	int i;

	for (i = 0; i < VMEM_MGR_BLOCKS; i++) {
		if (mgr->blocks[i].state == VMEM_BLOCK_FIXED_B)
			continue;

		mgr->blocks[i].state =
			(mode == VMEM_VIEW_4CH) ? VMEM_BLOCK_ACTIVE_A : VMEM_BLOCK_ACTIVE_B;
		mgr->blocks[i].current_view = mode;
	}
}

static ssize_t current_mode_show(struct kobject *kobj,
				 struct kobj_attribute *attr, char *buf)
{
	size_t count;

	mutex_lock(&vmem_mgr.lock);
	count = sysfs_emit(buf, "%s\n", vmem_view_name(vmem_mgr.mode));
	mutex_unlock(&vmem_mgr.lock);

	return count;
}

static ssize_t current_mode_store(struct kobject *kobj,
				  struct kobj_attribute *attr,
				  const char *buf, size_t count)
{
	enum vmem_view_mode mode;

	if (sysfs_streq(buf, "view_a"))
		mode = VMEM_VIEW_4CH;
	else if (sysfs_streq(buf, "view_b"))
		mode = VMEM_VIEW_2CH;
	else
		return -EINVAL;

	mutex_lock(&vmem_mgr.lock);
	vmem_mgr.mode = mode;
	vmem_mgr_update_blocks(&vmem_mgr, mode);
	vmem_mgr.active_phy_mask = (mode == VMEM_VIEW_4CH) ? 0xF : 0x3;
	mutex_unlock(&vmem_mgr.lock);

	return count;
}

static struct kobj_attribute current_mode_attr =
	__ATTR_RW(current_mode);

static ssize_t block_map_show(struct kobject *kobj,
			      struct kobj_attribute *attr, char *buf)
{
	ssize_t len = 0;
	int i;

	mutex_lock(&vmem_mgr.lock);
	for (i = 0; i < VMEM_MGR_BLOCKS; i++) {
		len += sysfs_emit_at(buf, len,
				     "block %03u: %s (%uMB)\n",
				     vmem_mgr.blocks[i].id,
				     vmem_block_state_name(vmem_mgr.blocks[i].state),
				     (i + 1) * VMEM_MGR_BLOCK_SIZE_MB);
	}
	mutex_unlock(&vmem_mgr.lock);

	return len;
}

static struct kobj_attribute block_map_attr =
	__ATTR_RO(block_map);

static ssize_t active_phy_mask_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	size_t count;

	mutex_lock(&vmem_mgr.lock);
	count = sysfs_emit(buf, "0x%x\n", vmem_mgr.active_phy_mask);
	mutex_unlock(&vmem_mgr.lock);

	return count;
}

static struct kobj_attribute active_phy_mask_attr =
	__ATTR_RO(active_phy_mask);

static struct attribute *vmem_mgr_attrs[] = {
	&current_mode_attr.attr,
	&block_map_attr.attr,
	&active_phy_mask_attr.attr,
	NULL,
};

static const struct attribute_group vmem_mgr_attr_group = {
	.attrs = vmem_mgr_attrs,
};

static void vmem_mgr_init_blocks(struct vmem_mgr *mgr)
{
	int i;

	for (i = 0; i < VMEM_MGR_BLOCKS; i++) {
		mgr->blocks[i].id = i;
		mgr->blocks[i].phys_addr = mgr->view_a_base +
					  (unsigned long)i *
					  (VMEM_MGR_BLOCK_SIZE_MB << 20);
		mgr->blocks[i].current_view = VMEM_VIEW_4CH;
		mgr->blocks[i].state = VMEM_BLOCK_ACTIVE_A;
		mutex_init(&mgr->blocks[i].lock);
	}

	for (i = 0; i < VMEM_MGR_FIXED_B_BLOCKS; i++)
		mgr->blocks[i].state = VMEM_BLOCK_FIXED_B;
}

static int __init vmem_mgr_init(void)
{
	int ret;

	mutex_init(&vmem_mgr.lock);
	vmem_mgr.mode = VMEM_VIEW_4CH;
	vmem_mgr.view_a_base = 0x0UL;
	vmem_mgr.view_b_base = 0x400000000UL;
	vmem_mgr.active_phy_mask = 0xF;
	vmem_mgr_init_blocks(&vmem_mgr);

	vmem_kobj = kobject_create_and_add("vmem_mgr", kernel_kobj);
	if (!vmem_kobj)
		return -ENOMEM;

	ret = sysfs_create_group(vmem_kobj, &vmem_mgr_attr_group);
	if (ret)
		goto err_sysfs;

	ret = vmem_mgr_numa_init(&vmem_mgr);
	if (ret)
		goto err_numa;

	ret = vmem_mgr_hotplug_init(&vmem_mgr);
	if (ret)
		goto err_hotplug;

	ret = vmem_mgr_reorder_init(&vmem_mgr);
	if (ret)
		goto err_reorder;

	pr_info("vmem_mgr: core scaffold initialized (mode=%s)\n",
		vmem_view_name(vmem_mgr.mode));
	return 0;

err_reorder:
	vmem_mgr_hotplug_exit(&vmem_mgr);
err_hotplug:
	vmem_mgr_numa_exit(&vmem_mgr);
err_numa:
	sysfs_remove_group(vmem_kobj, &vmem_mgr_attr_group);
err_sysfs:
	kobject_put(vmem_kobj);
	vmem_kobj = NULL;
	return ret;
}

static void __exit vmem_mgr_exit(void)
{
	vmem_mgr_reorder_exit(&vmem_mgr);
	vmem_mgr_hotplug_exit(&vmem_mgr);
	vmem_mgr_numa_exit(&vmem_mgr);
	if (vmem_kobj) {
		sysfs_remove_group(vmem_kobj, &vmem_mgr_attr_group);
		kobject_put(vmem_kobj);
		vmem_kobj = NULL;
	}
	pr_info("vmem_mgr: core scaffold exited\n");
}

module_init(vmem_mgr_init);
module_exit(vmem_mgr_exit);

MODULE_DESCRIPTION("VMEM Manager core scaffold");
MODULE_AUTHOR("OpenAI");
MODULE_LICENSE("GPL");
