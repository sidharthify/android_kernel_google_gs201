// SPDX-License-Identifier: GPL-2.0
/*
 * Janeiro platform device driver for the Google Edge TPU ML accelerator.
 *
 * Copyright (C) 2020 Google, Inc.
 */

#include <linux/device.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>

#include "edgetpu-config.h"
#include "edgetpu-internal.h"
#include "edgetpu-mobile-platform.h"
#include "janeiro-platform.h"

#include "edgetpu-mobile-platform.c"

static const struct of_device_id edgetpu_of_match[] = {
	/* TODO(b/190677977): remove  */
	{ .compatible = "google,darwinn", },
	{ .compatible = "google,edgetpu-gs201", },
	{ /* end of list */ },
};

MODULE_DEVICE_TABLE(of, edgetpu_of_match);

/*
 * Set shareability for enabling IO coherency in Janeiro
 */
static int janeiro_mmu_set_shareability(struct janeiro_platform_dev *jpdev, u32 reg_base)
{
	struct device *dev = jpdev->mobile_dev.edgetpu_dev.dev;
	void __iomem *addr = devm_ioremap(dev, reg_base, PAGE_SIZE);

	if (!addr) {
		dev_err(dev, "sysreg ioremap failed\n");
		return -ENOMEM;
	}

	jpdev->shareability = addr + EDGETPU_SYSREG_TPU_SHAREABILITY;
	writel_relaxed(SHAREABLE_WRITE | SHAREABLE_READ | INNER_SHAREABLE, jpdev->shareability);

	return 0;
}

/*
 * The shareability register lives in the TPU block and resets whenever pd-tpu
 * powers off, so set it again on every power-up.
 */
void janeiro_restore_shareability(struct edgetpu_dev *etdev)
{
	struct janeiro_platform_dev *jpdev = to_janeiro_dev(etdev);

	if (jpdev->shareability)
		writel_relaxed(SHAREABLE_WRITE | SHAREABLE_READ | INNER_SHAREABLE,
			       jpdev->shareability);
}

/* S2MPU control register; clearing ENABLE lets all traffic through */
#define S2MPU_REG_NS_CTRL0	0x0

/*
 * Under pKVM the hypervisor configures the TPU S2MPU whenever pd-tpu powers
 * up. Without it nothing does, and the S2MPU comes out of every power-up
 * enabled with no permissions. The TPU's DMA, including the SysMMU's page
 * table walks, then reads zeros, and every client access page-faults. Switch
 * it to pass-through after each power-up instead.
 */
void janeiro_open_s2mpu(struct edgetpu_dev *etdev)
{
	struct janeiro_platform_dev *jpdev = to_janeiro_dev(etdev);

	if (jpdev->s2mpu)
		writel(0, jpdev->s2mpu + S2MPU_REG_NS_CTRL0);
}

static void janeiro_map_s2mpu(struct janeiro_platform_dev *jpdev)
{
	struct device *dev = jpdev->mobile_dev.edgetpu_dev.dev;
	struct device_node *np;
	void __iomem *base;

	if (IS_ENABLED(CONFIG_PKVM_S2MPU))
		return;

	np = of_parse_phandle(dev->of_node, "s2mpu", 0);
	if (!np)
		return;
	base = devm_of_iomap(dev, np, 0, NULL);
	of_node_put(np);
	if (IS_ERR(base)) {
		dev_warn(dev, "failed to map s2mpu (%ld)\n", PTR_ERR(base));
		return;
	}
	jpdev->s2mpu = base;
}

static int janeiro_parse_set_dt_property(struct edgetpu_mobile_platform_dev *etmdev)
{
	int ret;
	u32 reg;
	struct edgetpu_dev *etdev = &etmdev->edgetpu_dev;
	struct device *dev = etdev->dev;

	ret = edgetpu_mobile_platform_set_fw_ctx_memory(etmdev);
	if (ret) {
		etdev_err(etdev, "Failed to initialize fw context memory: %d", ret);
		return ret;
	}

	if (!of_find_property(dev->of_node, "edgetpu,shareability", NULL)) {
		ret = -ENODEV;
		goto err;
	}
	ret = of_property_read_u32_index(dev->of_node, "edgetpu,shareability", 0, &reg);
	if (ret)
		goto err;
	ret = janeiro_mmu_set_shareability(to_janeiro_dev(etdev), reg);
err:
	if (ret)
		etdev_warn(etdev, "failed to enable shareability: %d", ret);

	return 0;
}

static int janeiro_platform_after_probe(struct edgetpu_mobile_platform_dev *etmdev)
{
	janeiro_map_s2mpu(to_janeiro_dev(&etmdev->edgetpu_dev));
	return janeiro_parse_set_dt_property(etmdev);
}

static int edgetpu_platform_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct janeiro_platform_dev *jpdev;
	struct edgetpu_mobile_platform_dev *etmdev;

	jpdev = devm_kzalloc(dev, sizeof(*jpdev), GFP_KERNEL);
	if (!jpdev)
		return -ENOMEM;

	etmdev = &jpdev->mobile_dev;
	etmdev->after_probe = janeiro_platform_after_probe;
	return edgetpu_mobile_platform_probe(pdev, etmdev);
}

static void edgetpu_platform_remove(struct platform_device *pdev)
{
	edgetpu_mobile_platform_remove(pdev);
}

#if IS_ENABLED(CONFIG_PM_SLEEP)

static int edgetpu_platform_suspend(struct device *dev)
{
	struct edgetpu_dev *etdev = dev_get_drvdata(dev);

	return edgetpu_pm_suspend(etdev);
}

static int edgetpu_platform_resume(struct device *dev)
{
	struct edgetpu_dev *etdev = dev_get_drvdata(dev);

	return edgetpu_pm_resume(etdev);
}

#endif /* IS_ENABLED(CONFIG_PM_SLEEP) */

static const struct dev_pm_ops edgetpu_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(edgetpu_platform_suspend,
				edgetpu_platform_resume)
};

static struct platform_driver edgetpu_platform_driver = {
	.probe = edgetpu_platform_probe,
	.remove = edgetpu_platform_remove,
	.driver = {
		.name = "edgetpu_platform",
		.of_match_table = edgetpu_of_match,
		.pm = &edgetpu_pm_ops,
	},
};

static int __init edgetpu_platform_init(void)
{
	int ret;

	ret = edgetpu_init();
	if (ret)
		return ret;
	return platform_driver_register(&edgetpu_platform_driver);
}

static void __exit edgetpu_platform_exit(void)
{
	platform_driver_unregister(&edgetpu_platform_driver);
	edgetpu_exit();
}

MODULE_DESCRIPTION("Google Edge TPU platform driver");
MODULE_LICENSE("GPL v2");
module_init(edgetpu_platform_init);
module_exit(edgetpu_platform_exit);
MODULE_FIRMWARE(EDGETPU_DEFAULT_FIRMWARE_NAME);
