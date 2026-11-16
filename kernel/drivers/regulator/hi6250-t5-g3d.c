// SPDX-License-Identifier: GPL-2.0-only
/* AGS2-L03 G3D switch, matching Huawei's GPL regulator type2 protocol.
 * LPM3 owns the rail, reset, isolation and clock-gate sequence.
 * Assisted-by: LLM.
 */
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>
#include <linux/slab.h>

struct t5_g3d {
	struct device *dev;
	struct mbox_client client;
	struct mbox_chan *channel;
	void __iomem *ack_regs;
	struct clk *clock;
	struct mutex lock;
	u32 message[8];
	bool enabled;
	bool peri_enabled;
};

static int t5_rail_command(struct t5_g3d *g, u32 command, u32 argument)
{
	u32 mode, ack0, ack1;
	int ret;
	lockdep_assert_held(&g->lock);
	g->message[0] = command;
	g->message[1] = argument;
	ret = mbox_send_message(g->channel, g->message);
	if (ret < 0)
		return ret;
	/* The controller owns this channel; only read its reply here. It does
	 * not forward reply payloads to clients, so use the channel's ACK bank.
	 * Channel14 uses manual ACK, as Huawei's RPROC_SYNC_SEND does.
	 */
	ret = readl_poll_timeout(g->ack_regs + 0x10, mode, mode & BIT(7), 50, 300000);
	if (!ret) {
		ack0 = readl(g->ack_regs + 0x20);
		ack1 = readl(g->ack_regs + 0x24);
		dev_info(g->dev, "LPM3 command=%08x ack=%08x/%08x\n", command, ack0, ack1);
		if (ack0 != command || (ack1 >> 24))
			ret = -EIO;
	}
	mbox_client_txdone(g->channel, ret);
	return ret;
}

static int t5_g3d_command(struct t5_g3d *g, u32 command)
{
	return t5_rail_command(g, command, 0);
}

static int t5_g3d_enable(struct regulator_dev *rdev)
{
	struct t5_g3d *g = rdev_get_drvdata(rdev);
	int ret;
	mutex_lock(&g->lock);
	ret = t5_g3d_command(g, 0x00030004);
	if (!ret) {
		g->enabled = true;
		ret = clk_set_rate(g->clock, 120000000);
		if (ret) {
			dev_err(g->dev, "120MHz request failed: %d\n", ret);
			if (!t5_g3d_command(g, 0x00030104))
				g->enabled = false;
		} else {
			dev_info(g->dev, "G3D enabled; firmware clock=%lu Hz\n", clk_get_rate(g->clock));
		}
	}
	mutex_unlock(&g->lock);
	return ret;
}

static int t5_g3d_disable(struct regulator_dev *rdev)
{
	struct t5_g3d *g = rdev_get_drvdata(rdev);
	int ret;
	mutex_lock(&g->lock);
	ret = t5_g3d_command(g, 0x00030104);
	if (!ret)
		g->enabled = false;
	mutex_unlock(&g->lock);
	return ret;
}

static int t5_g3d_is_enabled(struct regulator_dev *rdev)
{
	struct t5_g3d *g = rdev_get_drvdata(rdev);
	return READ_ONCE(g->enabled);
}

static const struct regulator_ops t5_g3d_ops = {
	.enable = t5_g3d_enable,
	.disable = t5_g3d_disable,
	.is_enabled = t5_g3d_is_enabled,
};
static const struct regulator_desc t5_g3d_desc = {
	.name = "t5-g3d", .id = 14, .type = REGULATOR_VOLTAGE,
	.ops = &t5_g3d_ops, .owner = THIS_MODULE,
};

/* sys_peri and G3D are independently voted outputs on one physical
 * channel. Regulator core owns the shared-consumer use_count. */
static int t5_peri_set(struct regulator_dev *rdev, bool on)
{
	struct t5_g3d *g = rdev_get_drvdata(rdev);
	int ret;

	mutex_lock(&g->lock);
	ret = t5_rail_command(g, on ? 0x000c0006 : 0x000c0106, 0x00000601);
	if (!ret)
		WRITE_ONCE(g->peri_enabled, on);
	mutex_unlock(&g->lock);
	return ret;
}

static int t5_peri_enable(struct regulator_dev *rdev)
{
	return t5_peri_set(rdev, true);
}

static int t5_peri_disable(struct regulator_dev *rdev)
{
	return t5_peri_set(rdev, false);
}

static int t5_peri_is_enabled(struct regulator_dev *rdev)
{
	struct t5_g3d *g = rdev_get_drvdata(rdev);

	return READ_ONCE(g->peri_enabled);
}

static const struct regulator_ops t5_peri_ops = {
	.enable = t5_peri_enable,
	.disable = t5_peri_disable,
	.is_enabled = t5_peri_is_enabled,
};

static const struct regulator_desc t5_peri_desc = {
	.name = "t5-sys-peri", .id = 1, .type = REGULATOR_VOLTAGE,
	.ops = &t5_peri_ops, .owner = THIS_MODULE,
};

static int t5_register_peri(struct device *dev, struct t5_g3d *g)
{
	struct regulator_config cfg = { .dev = dev, .driver_data = g };
	struct regulator_dev *reg;
	struct device_node *node = of_get_child_by_name(dev->of_node, "sys-peri");
	int ret = 0;

	/* Existing G3D-only boards do not need this child. */
	if (!node)
		return 0;
	if (of_device_is_available(node)) {
		cfg.of_node = node;
		cfg.init_data = of_get_regulator_init_data(dev, node, &t5_peri_desc);
		if (!cfg.init_data) {
			ret = -EINVAL;
		} else {
			reg = devm_regulator_register(dev, &t5_peri_desc, &cfg);
			ret = PTR_ERR_OR_ZERO(reg);
		}
	}
	of_node_put(node);
	return ret;
}

static void t5_g3d_free_channel(void *channel)
{
	mbox_free_channel(channel);
}

static int t5_g3d_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct regulator_config config = {};
	struct of_phandle_args spec;
	struct regulator_dev *rdev;
	struct resource resource;
	struct t5_g3d *g;
	int ret;
	g = devm_kzalloc(dev, sizeof(*g), GFP_KERNEL);
	if (!g)
		return -ENOMEM;
	g->dev = dev;
	mutex_init(&g->lock);
	g->clock = devm_clk_get(dev, NULL);
	if (IS_ERR(g->clock))
		return dev_err_probe(dev, PTR_ERR(g->clock), "GPU firmware clock missing\n");
	ret = of_parse_phandle_with_args(dev->of_node, "mboxes", "#mbox-cells", 0, &spec);
	if (ret)
		return ret;
	if (spec.args_count != 3 || spec.args[0] != 14 || spec.args[1] != 3 || spec.args[2] != 1) {
		of_node_put(spec.np);
		return -EINVAL;
	}
	ret = of_address_to_resource(spec.np, 0, &resource);
	of_node_put(spec.np);
	if (ret || resource_size(&resource) < 15 * 0x40)
		return -EINVAL;
	/* Shared mapping of our own ACK registers; the mailbox controller
	 * retains ownership of the IPC MMIO resource and channel arbitration.
	 */
	g->ack_regs = devm_ioremap(dev, resource.start + 14 * 0x40, 0x40);
	if (!g->ack_regs)
		return -ENOMEM;
	g->client.dev = dev;
	g->client.knows_txdone = true;
	g->channel = mbox_request_channel(&g->client, 0);
	if (IS_ERR(g->channel))
		return dev_err_probe(dev, PTR_ERR(g->channel), "LPM3 regulator mailbox missing\n");
	ret = devm_add_action_or_reset(dev, t5_g3d_free_channel, g->channel);
	if (ret)
		return ret;
	config.dev = dev;
	config.of_node = dev->of_node;
	config.driver_data = g;
	config.init_data = of_get_regulator_init_data(dev, dev->of_node, &t5_g3d_desc);
	if (!config.init_data)
		return -EINVAL;
	rdev = devm_regulator_register(dev, &t5_g3d_desc, &config);
	if (IS_ERR(rdev))
		return PTR_ERR(rdev);
	return t5_register_peri(dev, g);
}

static const struct of_device_id t5_g3d_match[] = {
	{ .compatible = "huawei,ags2l03-g3d-regulator" }, {}
};
static struct platform_driver t5_g3d_driver = {
	.probe = t5_g3d_probe,
	.driver = { .name = "hi6250-t5-g3d", .of_match_table = t5_g3d_match },
};
builtin_platform_driver(t5_g3d_driver);
