// SPDX-License-Identifier: GPL-2.0-only
/*
 * S2MPU pass-through for kernels without pKVM
 *
 * Only the pKVM S2MPU driver binds google,s2mpu nodes, and only the pKVM
 * hypervisor programs the S2MPUs, each time their power domain powers up.
 * Without pKVM, two things go wrong:
 *
 *  - exynos-pd keeps a domain that was on at boot powered until every consumer
 *    in the device tree has probed (sync_state), and the S2MPU nodes never do.
 *    Every domain with an S2MPU (display, codecs, camera, TPU, ...) stays
 *    powered forever.
 *  - An S2MPU comes out of every power-up of its domain enabled and with no
 *    permissions, blocking its master's DMA.
 *
 * Bind the nodes so their domains can power off, and switch each S2MPU to
 * pass-through after every power-up of its domain. This gives up the S2MPUs'
 * DMA isolation, which a kernel without pKVM can't offer anyway.
 *
 * S2MPUs outside a power domain keep what the bootloader or their master's
 * driver sets up (GSA, HSI0), so they are bound and left alone. Their
 * registers aren't requested here either, as those drivers map them too.
 */

#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>

/* S2MPU control register; clearing ENABLE lets all traffic through */
#define S2MPU_REG_NS_CTRL0	0x0

struct s2mpu_passthrough {
	void __iomem *base;
	struct notifier_block nb;
	bool powered;	/* the domain is on */
	bool sleeping;	/* between suspend_late and resume_early */
};

static int s2mpu_passthrough_genpd_event(struct notifier_block *nb, unsigned long action,
					 void *data)
{
	struct s2mpu_passthrough *s = container_of(nb, struct s2mpu_passthrough, nb);

	switch (action) {
	case GENPD_NOTIFY_ON:
		/*
		 * Sent once the domain is fully powered and its secure state
		 * restored. During system resume the domain comes back in the
		 * noirq phase, where touching the S2MPU raises an SError, so
		 * leave that to resume_early.
		 */
		s->powered = true;
		if (!READ_ONCE(s->sleeping))
			writel(0, s->base + S2MPU_REG_NS_CTRL0);
		break;
	case GENPD_NOTIFY_OFF:
		s->powered = false;
		break;
	}

	return NOTIFY_OK;
}

static void s2mpu_passthrough_remove_notifier(void *dev)
{
	dev_pm_genpd_remove_notifier(dev);
}

static int s2mpu_passthrough_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct s2mpu_passthrough *s;
	struct resource *res;
	int ret;

	if (!dev->pm_domain || of_property_read_bool(dev->of_node, "deny-all"))
		return 0;

	s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;
	s->base = devm_ioremap(dev, res->start, resource_size(res));
	if (!s->base)
		return -ENOMEM;

	platform_set_drvdata(pdev, s);
	s->powered = true;	/* until the put below powers it off */
	s->nb.notifier_call = s2mpu_passthrough_genpd_event;
	ret = dev_pm_genpd_add_notifier(dev, &s->nb);
	if (ret)
		return dev_err_probe(dev, ret, "failed to add genpd notifier\n");
	ret = devm_add_action_or_reset(dev, s2mpu_passthrough_remove_notifier, dev);
	if (ret)
		return ret;

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	/* let genpd re-evaluate the domain now that this consumer is idle */
	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return ret;
	pm_runtime_put_sync(dev);

	return 0;
}

static int s2mpu_passthrough_suspend_late(struct device *dev)
{
	struct s2mpu_passthrough *s = dev_get_drvdata(dev);

	if (s)
		WRITE_ONCE(s->sleeping, true);
	return 0;
}

static int s2mpu_passthrough_resume_early(struct device *dev)
{
	struct s2mpu_passthrough *s = dev_get_drvdata(dev);

	if (!s)
		return 0;
	/* the same point pKVM's S2MPU driver restores its S2MPUs at */
	if (s->powered)
		writel(0, s->base + S2MPU_REG_NS_CTRL0);
	WRITE_ONCE(s->sleeping, false);
	return 0;
}

static const struct dev_pm_ops s2mpu_passthrough_pm_ops = {
	LATE_SYSTEM_SLEEP_PM_OPS(s2mpu_passthrough_suspend_late,
				 s2mpu_passthrough_resume_early)
};

static const struct of_device_id s2mpu_passthrough_of_match[] = {
	{ .compatible = "google,s2mpu" },
	{},
};
MODULE_DEVICE_TABLE(of, s2mpu_passthrough_of_match);

static struct platform_driver s2mpu_passthrough_driver = {
	.probe = s2mpu_passthrough_probe,
	.driver = {
		.name = "s2mpu-passthrough",
		.of_match_table = s2mpu_passthrough_of_match,
		.pm = pm_sleep_ptr(&s2mpu_passthrough_pm_ops),
	},
};
module_platform_driver(s2mpu_passthrough_driver);

MODULE_DESCRIPTION("S2MPU pass-through for kernels without pKVM");
MODULE_LICENSE("GPL");
