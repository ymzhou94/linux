// SPDX-License-Identifier: GPL-2.0
/*
 * LPDDR6 V-Topology Manager (prototype)
 *
 * Provides a software-defined view switcher and block map for LPDDR6
 * aliasing topologies, exposing sysfs knobs described in the design docs.
 */

#include <linux/bus.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/string.h>

#define VTOPOLOGY_BLOCKS            128
#define VTOPOLOGY_BLOCK_SIZE_MB     128
#define VTOPOLOGY_FIXED_B_BLOCKS    8

enum vmem_mode {
	VMEM_MODE_A = 0,
	VMEM_MODE_B,
};

enum vmem_block_state {
	VMEM_BLOCK_ACTIVE_A = 0,
	VMEM_BLOCK_MIGRATING,
	VMEM_BLOCK_ACTIVE_B,
	VMEM_BLOCK_FIXED_B,
};

struct vmem_block {
	enum vmem_block_state state;
};

struct vmem_mgr {
	struct mutex lock;
	enum vmem_mode mode;
	struct vmem_block blocks[VTOPOLOGY_BLOCKS];
};

static struct vmem_mgr vmem_mgr;

static const char *vmem_mode_name(enum vmem_mode mode)
{
	switch (mode) {
	case VMEM_MODE_A:
		return "view_a";
	case VMEM_MODE_B:
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

static void vmem_update_blocks(enum vmem_mode mode)
{
	int i;

	for (i = 0; i < VTOPOLOGY_BLOCKS; i++) {
		if (vmem_mgr.blocks[i].state == VMEM_BLOCK_FIXED_B)
			continue;

		vmem_mgr.blocks[i].state =
			(mode == VMEM_MODE_A) ? VMEM_BLOCK_ACTIVE_A : VMEM_BLOCK_ACTIVE_B;
	}
}

static ssize_t current_mode_show(struct bus_type *bus, char *buf)
{
	ssize_t count;

	mutex_lock(&vmem_mgr.lock);
	count = sysfs_emit(buf, "%s\n", vmem_mode_name(vmem_mgr.mode));
	mutex_unlock(&vmem_mgr.lock);

	return count;
}

static ssize_t current_mode_store(struct bus_type *bus, const char *buf,
				  size_t count)
{
	enum vmem_mode mode;

	if (sysfs_streq(buf, "view_a"))
		mode = VMEM_MODE_A;
	else if (sysfs_streq(buf, "view_b"))
		mode = VMEM_MODE_B;
	else
		return -EINVAL;

	mutex_lock(&vmem_mgr.lock);
	vmem_mgr.mode = mode;
	vmem_update_blocks(mode);
	mutex_unlock(&vmem_mgr.lock);

	return count;
}

static BUS_ATTR_RW(current_mode);

static ssize_t block_map_show(struct bus_type *bus, char *buf)
{
	int i;
	ssize_t len = 0;

	mutex_lock(&vmem_mgr.lock);
	for (i = 0; i < VTOPOLOGY_BLOCKS; i++) {
		len += sysfs_emit_at(buf, len,
				     "block %03d: %s (%uMB)\n",
				     i,
				     vmem_block_state_name(vmem_mgr.blocks[i].state),
				     (i + 1) * VTOPOLOGY_BLOCK_SIZE_MB);
	}
	mutex_unlock(&vmem_mgr.lock);

	return len;
}

static BUS_ATTR_RO(block_map);

static struct attribute *vmem_bus_attrs[] = {
	&bus_attr_current_mode.attr,
	&bus_attr_block_map.attr,
	NULL,
};

ATTRIBUTE_GROUPS(vmem_bus);

static struct bus_type vmem_bus_type = {
	.name = "vmem",
	.bus_groups = vmem_bus_groups,
};

static void vmem_init_blocks(void)
{
	int i;

	for (i = 0; i < VTOPOLOGY_BLOCKS; i++)
		vmem_mgr.blocks[i].state = VMEM_BLOCK_ACTIVE_A;

	for (i = 0; i < VTOPOLOGY_FIXED_B_BLOCKS; i++)
		vmem_mgr.blocks[i].state = VMEM_BLOCK_FIXED_B;
}

static int __init vmem_bus_init(void)
{
	int ret;

	mutex_init(&vmem_mgr.lock);
	vmem_mgr.mode = VMEM_MODE_A;
	vmem_init_blocks();

	ret = bus_register(&vmem_bus_type);
	if (ret)
		return ret;

	pr_info("lpddr6 vtopology: bus registered (mode=%s)\n",
		vmem_mode_name(vmem_mgr.mode));
	return 0;
}

static void __exit vmem_bus_exit(void)
{
	bus_unregister(&vmem_bus_type);
}

module_init(vmem_bus_init);
module_exit(vmem_bus_exit);

MODULE_DESCRIPTION("LPDDR6 V-Topology Manager (prototype)");
MODULE_AUTHOR("OpenAI");
MODULE_LICENSE("GPL");
