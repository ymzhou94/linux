// SPDX-License-Identifier: GPL-2.0-only
#include <linux/device.h>
#include <linux/memory_hotplug.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

enum rk_ddr_interleave_mode {
	RK_DDR_INTERLEAVE_4CH,
	RK_DDR_INTERLEAVE_2CH,
	RK_DDR_INTERLEAVE_NONE,
};

struct rk_ddr_interleave_region {
	struct resource res;
	u32 nid;
	enum rk_ddr_interleave_mode mode;
	bool boot_region;
};

struct rk_ddr_interleave {
	struct device *dev;
	struct rk_ddr_interleave_region *regions;
	int region_count;
	struct rk_ddr_interleave_region *active;
	struct mutex lock;
};

static const char *rk_ddr_interleave_mode_name(enum rk_ddr_interleave_mode mode)
{
	switch (mode) {
	case RK_DDR_INTERLEAVE_4CH:
		return "4ch";
	case RK_DDR_INTERLEAVE_2CH:
		return "2ch";
	case RK_DDR_INTERLEAVE_NONE:
		return "none";
	default:
		return "unknown";
	}
}

static int rk_ddr_interleave_parse_mode(const char *mode,
					enum rk_ddr_interleave_mode *out)
{
	if (!mode)
		return -EINVAL;

	if (!strcmp(mode, "4ch")) {
		*out = RK_DDR_INTERLEAVE_4CH;
		return 0;
	}

	if (!strcmp(mode, "2ch")) {
		*out = RK_DDR_INTERLEAVE_2CH;
		return 0;
	}

	if (!strcmp(mode, "none")) {
		*out = RK_DDR_INTERLEAVE_NONE;
		return 0;
	}

	return -EINVAL;
}

static int rk_ddr_interleave_apply_mode(struct rk_ddr_interleave *ddr,
					enum rk_ddr_interleave_mode mode)
{
	dev_info(ddr->dev, "request DDR interleave mode %s\n",
		 rk_ddr_interleave_mode_name(mode));
	return 0;
}

static int rk_ddr_interleave_add_region(struct rk_ddr_interleave *ddr,
					struct rk_ddr_interleave_region *region)
{
	return add_memory_driver_managed(region->nid, region->res.start,
					 resource_size(&region->res),
					 "System RAM (RK3588 DDR Interleave)",
					 MHP_NONE);
}

static int rk_ddr_interleave_remove_region(struct rk_ddr_interleave *ddr,
					   struct rk_ddr_interleave_region *region)
{
	if (!IS_ENABLED(CONFIG_MEMORY_HOTREMOVE))
		return -EOPNOTSUPP;

	return offline_and_remove_memory(region->res.start,
					 resource_size(&region->res));
}

static int rk_ddr_interleave_switch(struct rk_ddr_interleave *ddr,
				    struct rk_ddr_interleave_region *target)
{
	struct rk_ddr_interleave_region *prev;
	int ret;

	if (ddr->active == target)
		return 0;

	mutex_lock(&ddr->lock);
	prev = ddr->active;
	if (prev && prev->boot_region) {
		mutex_unlock(&ddr->lock);
		return -EBUSY;
	}

	if (prev && !prev->boot_region) {
		ret = rk_ddr_interleave_remove_region(ddr, prev);
		if (ret)
			goto out_unlock;
	}

	ret = rk_ddr_interleave_apply_mode(ddr, target->mode);
	if (ret)
		goto out_restore;

	if (!target->boot_region) {
		ret = rk_ddr_interleave_add_region(ddr, target);
		if (ret)
			goto out_restore;
	}

	ddr->active = target;
	mutex_unlock(&ddr->lock);
	return 0;

out_restore:
	if (prev && !prev->boot_region) {
		int restore_ret;

		restore_ret = rk_ddr_interleave_add_region(ddr, prev);
		if (restore_ret)
			dev_err(ddr->dev,
				"failed to restore previous region: %d\n",
				restore_ret);
	}
out_unlock:
	mutex_unlock(&ddr->lock);
	return ret;
}

static ssize_t interleave_mode_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct rk_ddr_interleave *ddr = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n",
			  rk_ddr_interleave_mode_name(ddr->active->mode));
}

static struct rk_ddr_interleave_region *
rk_ddr_interleave_region_by_mode(struct rk_ddr_interleave *ddr,
				 enum rk_ddr_interleave_mode mode)
{
	int i;

	for (i = 0; i < ddr->region_count; i++) {
		if (ddr->regions[i].mode == mode)
			return &ddr->regions[i];
	}

	return NULL;
}

static ssize_t interleave_mode_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct rk_ddr_interleave *ddr = dev_get_drvdata(dev);
	enum rk_ddr_interleave_mode mode;
	struct rk_ddr_interleave_region *target;
	char mode_buf[8];
	int ret;

	if (count >= sizeof(mode_buf))
		return -EINVAL;

	memcpy(mode_buf, buf, count);
	mode_buf[count] = '\0';
	strim(mode_buf);

	ret = rk_ddr_interleave_parse_mode(mode_buf, &mode);
	if (ret)
		return ret;

	target = rk_ddr_interleave_region_by_mode(ddr, mode);
	if (!target)
		return -EINVAL;

	ret = rk_ddr_interleave_switch(ddr, target);
	if (ret)
		return ret;

	return count;
}

static DEVICE_ATTR_RW(interleave_mode);

static int rk_ddr_interleave_parse_regions(struct rk_ddr_interleave *ddr,
					   struct device_node *np)
{
	struct device_node *child;
	int count = 0;
	int i = 0;

	count = of_get_child_count(np);
	if (!count)
		return -EINVAL;

	ddr->regions = devm_kcalloc(ddr->dev, count, sizeof(*ddr->regions),
				    GFP_KERNEL);
	if (!ddr->regions)
		return -ENOMEM;

	for_each_available_child_of_node(np, child) {
		struct rk_ddr_interleave_region *region = &ddr->regions[i];
		const char *mode;
		u32 nid;
		int ret;

		ret = of_address_to_resource(child, 0, &region->res);
		if (ret) {
			of_node_put(child);
			return ret;
		}

		ret = of_property_read_u32(child, "numa-node-id", &nid);
		if (ret) {
			of_node_put(child);
			return ret;
		}

		mode = of_get_property(child, "rockchip,interleave-mode", NULL);
		ret = rk_ddr_interleave_parse_mode(mode, &region->mode);
		if (ret) {
			of_node_put(child);
			return ret;
		}

		if (!IS_ALIGNED(region->res.start,
				memory_block_size_bytes()) ||
		    !IS_ALIGNED(resource_size(&region->res),
				memory_block_size_bytes())) {
			dev_err(ddr->dev,
				"region %pOF not aligned to memory block size\n",
				child);
			of_node_put(child);
			return -EINVAL;
		}

		region->nid = nid;
		region->boot_region = of_property_read_bool(child,
							    "rockchip,boot-region");

		if (region->boot_region)
			ddr->active = region;

		i++;
	}

	ddr->region_count = i;
	if (!ddr->active)
		ddr->active = &ddr->regions[0];

	return 0;
}

static int rk_ddr_interleave_probe(struct platform_device *pdev)
{
	struct rk_ddr_interleave *ddr;
	struct device *dev = &pdev->dev;
	int ret;

	ddr = devm_kzalloc(dev, sizeof(*ddr), GFP_KERNEL);
	if (!ddr)
		return -ENOMEM;

	ddr->dev = dev;
	mutex_init(&ddr->lock);
	platform_set_drvdata(pdev, ddr);

	ret = rk_ddr_interleave_parse_regions(ddr, dev->of_node);
	if (ret)
		return ret;

	ret = device_create_file(dev, &dev_attr_interleave_mode);
	if (ret)
		return ret;

	dev_info(dev, "DDR interleave control ready, active %s\n",
		 rk_ddr_interleave_mode_name(ddr->active->mode));
	return 0;
}

static int rk_ddr_interleave_remove(struct platform_device *pdev)
{
	struct rk_ddr_interleave *ddr = platform_get_drvdata(pdev);

	device_remove_file(&pdev->dev, &dev_attr_interleave_mode);
	mutex_destroy(&ddr->lock);

	return 0;
}

static const struct of_device_id rk_ddr_interleave_of_match[] = {
	{ .compatible = "rockchip,rk3588-ddr-interleave" },
	{ },
};
MODULE_DEVICE_TABLE(of, rk_ddr_interleave_of_match);

static struct platform_driver rk_ddr_interleave_driver = {
	.probe = rk_ddr_interleave_probe,
	.remove = rk_ddr_interleave_remove,
	.driver = {
		.name = "rk3588-ddr-interleave",
		.of_match_table = rk_ddr_interleave_of_match,
	},
};
module_platform_driver(rk_ddr_interleave_driver);

MODULE_DESCRIPTION("RK3588 DDR interleave hotplug control");
MODULE_LICENSE("GPL");
