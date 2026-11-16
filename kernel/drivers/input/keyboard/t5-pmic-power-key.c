// SPDX-License-Identifier: GPL-2.0-only
/* AGS2-L03 Hi6555C power key. Its AP interrupt shares a wire with the codec. */
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/suspend.h>

#define PMIC_MASK0	(0x130 * 4)
#define PMIC_MASK1	(0x131 * 4)
#define PMIC_PENDING0	(0x1b0 * 4)
#define PMIC_PENDING1	(0x1b1 * 4)
#define PMIC_VERSION4	(0x004 * 4)
#define SIM_HPD_EDGES	GENMASK(5, 2)
#define KEY_FALL	BIT(7)
#define KEY_RISE	BIT(6)
#define KEY_BITS	(KEY_FALL | KEY_RISE)

struct t5_power {
	struct device *dev;
	struct input_dev *input;
	struct gpio_desc *line;
	void __iomem *base;
	struct mutex config_lock;
	spinlock_t event_lock;
	int irq;
	u32 original_key_mask;
	bool armed;
	bool pressed;
	bool suspending;
	/* A wake press, including its release after userspace resumes. */
	bool wake_press;
	u32 down, up, wake, foreign;
	u32 sim_acks, sim_before, sim_cleared, sim_after;
};

static void t5_key_blocked(struct t5_power *key, const char *stage, int asserted)
{
	u32 pending = readl(key->base + PMIC_PENDING0);
	u32 mask = readl(key->base + PMIC_MASK0);

	dev_warn_ratelimited(key->dev,
			"%s blocked: gpio23.7 asserted=%d IRQ=%d key_pending=%02x key_mask=%02x pressed=%u\n",
			stage, asserted, key->irq, (u32)(pending & KEY_BITS),
			(u32)(mask & KEY_BITS), READ_ONCE(key->pressed));
}

static u32 t5_key_mask(struct t5_power *key, u32 value)
{
	u32 before = readl(key->base + PMIC_MASK0);
	u32 after = (before & ~KEY_BITS) | value;

	if (after != before)
		writel(after, key->base + PMIC_MASK0);
	return readl(key->base + PMIC_MASK0) & KEY_BITS;
}

/*
 * Stock Hi6555 IRQ1[5:2] are the two SIM tray rising/falling latches.
 * The stock AP cascade acknowledges dispatched latches by writing ones.
 * This board has no Linux SIM consumer. Preserve all masked events and do
 * not consume a latch unless it is holding the shared power-key wire low.
 * Caller owns event_lock or has not yet enabled the handler.
 */
static bool t5_key_release_sim_irq(struct t5_power *key, const char *stage)
{
	u32 before, active, clear;

	if (gpiod_get_value(key->line) != 1 ||
	    (readl(key->base + PMIC_PENDING0) & ~readl(key->base + PMIC_MASK0) & 0xff))
		return false;
	before = readl(key->base + PMIC_PENDING1) & 0xff;
	active = before & ~readl(key->base + PMIC_MASK1);
	clear = active & SIM_HPD_EDGES;
	if (!clear || active != clear)
		return false;
	writel(clear, key->base + PMIC_PENDING1);
	key->sim_after = readl(key->base + PMIC_PENDING1) & 0xff;
	key->sim_before = before;
	key->sim_cleared = clear;
	key->sim_acks++;
	dev_info_ratelimited(key->dev,
			"%s SIM hotplug IRQ1 before=%02x ack=%02x after=%02x GPIO asserted=%d\n",
			stage, before, clear, key->sim_after, gpiod_get_value(key->line));
	return true;
}

static irqreturn_t t5_key_irq(int irq, void *arg)
{
	struct t5_power *key = arg;
	unsigned long flags;
	u32 events;

	spin_lock_irqsave(&key->event_lock, flags);
	events = readl(key->base + PMIC_PENDING0) & KEY_BITS;
	if (!events) {
		key->foreign++;
		goto done;
	}
	/* Stock W1C register. Acknowledge only the two events we own. */
	writel(events, key->base + PMIC_PENDING0);
	readl(key->base + PMIC_PENDING0);
	if (events & KEY_FALL && !key->pressed) {
		key->pressed = true;
		key->down++;
		key->wake_press = READ_ONCE(key->suspending) && device_may_wakeup(key->dev);
		if (key->wake_press) {
			key->wake++;
			pm_wakeup_hard_event(key->dev);
		} else {
			input_report_key(key->input, KEY_POWER, 1);
			input_sync(key->input);
		}
	}
	/* Stock reports DOWN then UP when both latches are set. */
	if (events & KEY_RISE && key->pressed) {
		key->pressed = false;
		key->up++;
		if (!key->wake_press) {
			input_report_key(key->input, KEY_POWER, 0);
			input_sync(key->input);
		}
		key->wake_press = false;
	}
done:
	t5_key_release_sim_irq(key, "irq");
	spin_unlock_irqrestore(&key->event_lock, flags);
	return IRQ_HANDLED;
}

static void t5_key_disarm(struct t5_power *key)
{
	if (!key->armed)
		return;
	disable_irq(key->irq);
	t5_key_mask(key, key->original_key_mask);
	if (key->pressed) {
		if (!key->wake_press) {
			input_report_key(key->input, KEY_POWER, 0);
			input_sync(key->input);
		}
		key->wake_press = false;
		key->pressed = false;
	}
	key->armed = false;
}

static void t5_key_shutdown(void *arg)
{
	struct t5_power *key = arg;

	mutex_lock(&key->config_lock);
	t5_key_disarm(key);
	mutex_unlock(&key->config_lock);
}

static int t5_key_arm(struct t5_power *key)
{
	u32 pending;
	int active;

	if (key->armed)
		return 0;
	/* Discard stale PON edges from firmware, not codec/charger/RTC events. */
	pending = readl(key->base + PMIC_PENDING0) & KEY_BITS;
	if (pending) {
		writel(pending, key->base + PMIC_PENDING0);
		readl(key->base + PMIC_PENDING0);
	}
	/* The external pin can trail the AP's W1C register readback. */
	if (t5_key_release_sim_irq(key, "arm"))
		usleep_range(1000, 2000);
	active = gpiod_get_value_cansleep(key->line);
	if (active) {
		t5_key_blocked(key, "arm", active);
		return active < 0 ? active : -EBUSY;
	}
	key->original_key_mask = readl(key->base + PMIC_MASK0) & KEY_BITS;
	key->armed = true;
	enable_irq(key->irq);
	if (t5_key_mask(key, 0)) {
		t5_key_disarm(key);
		return -EIO;
	}
	return 0;
}

static ssize_t armed_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct t5_power *key = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(key->armed));
}

static ssize_t armed_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct t5_power *key = dev_get_drvdata(dev);
	bool on;
	int ret = kstrtobool(buf, &on);

	if (ret)
		return ret;
	mutex_lock(&key->config_lock);
	if (on)
		ret = t5_key_arm(key);
	else
		t5_key_disarm(key);
	mutex_unlock(&key->config_lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(armed);

static ssize_t status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct t5_power *key = dev_get_drvdata(dev);
	u32 mask0 = readl(key->base + PMIC_MASK0) & 0xff;
	u32 mask1 = readl(key->base + PMIC_MASK0 + 4) & 0xff;
	u32 pending0 = readl(key->base + PMIC_PENDING0) & 0xff;
	u32 pending1 = readl(key->base + PMIC_PENDING0 + 4) & 0xff;

	return sysfs_emit(buf, "irq=%d irq_name=%s armed=%u asserted=%d masks=%02x/%02x pending=%02x/%02x down=%u up=%u wake=%u foreign=%u sim_acks=%u sim_last=%02x/%02x/%02x\n",
			  key->irq, dev_name(dev), READ_ONCE(key->armed), gpiod_get_value_cansleep(key->line),
			  mask0, mask1, pending0, pending1, READ_ONCE(key->down),
			  READ_ONCE(key->up), READ_ONCE(key->wake), READ_ONCE(key->foreign),
			  READ_ONCE(key->sim_acks), READ_ONCE(key->sim_before),
			  READ_ONCE(key->sim_cleared), READ_ONCE(key->sim_after));
}
static DEVICE_ATTR_RO(status);

static struct attribute *t5_key_attrs[] = { &dev_attr_armed.attr, &dev_attr_status.attr, NULL };
ATTRIBUTE_GROUPS(t5_key);

static int t5_key_suspend(struct device *dev)
{
	struct t5_power *key = dev_get_drvdata(dev);
	unsigned long flags;
	bool released;
	int ret;

	if (pm_suspend_target_state != PM_SUSPEND_TO_IDLE || !key->armed ||
	    !device_may_wakeup(dev))
		return -EOPNOTSUPP;
	WRITE_ONCE(key->suspending, true);
	/* A stuck shared line cannot deliver another falling edge. */
	spin_lock_irqsave(&key->event_lock, flags);
	released = t5_key_release_sim_irq(key, "suspend");
	spin_unlock_irqrestore(&key->event_lock, flags);
	if (released)
		usleep_range(1000, 2000);
	ret = gpiod_get_value_cansleep(key->line);
	if (ret || READ_ONCE(key->pressed) || (readl(key->base + PMIC_MASK0) & KEY_BITS)) {
		t5_key_blocked(key, "suspend", ret);
		WRITE_ONCE(key->suspending, false);
		return ret < 0 ? ret : -EBUSY;
	}
	return 0;
}

static int t5_key_resume(struct device *dev)
{
	struct t5_power *key = dev_get_drvdata(dev);

	WRITE_ONCE(key->suspending, false);
	return 0;
}
static DEFINE_SIMPLE_DEV_PM_OPS(t5_key_pm, t5_key_suspend, t5_key_resume);

static int t5_key_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct t5_power *key;
	struct device_node *pmic;
	struct resource res;
	int ret;

	if (!of_machine_is_compatible("huawei,agassi2"))
		return -ENODEV;
	key = devm_kzalloc(dev, sizeof(*key), GFP_KERNEL);
	if (!key)
		return -ENOMEM;
	key->dev = dev;
	mutex_init(&key->config_lock);
	spin_lock_init(&key->event_lock);
	pmic = of_parse_phandle(dev->of_node, "huawei,pmic", 0);
	if (!pmic)
		return -EINVAL;
	ret = of_address_to_resource(pmic, 0, &res);
	of_node_put(pmic);
	if (ret || res.start != 0xfff34000 || resource_size(&res) != 0x1000)
		return -EINVAL;
	/* Shared mapping, as in the daily read-only battery telemetry driver. */
	key->base = devm_ioremap(dev, res.start, resource_size(&res));
	if (!key->base)
		return -ENOMEM;
	if (readb(key->base + PMIC_VERSION4) != 0x01)
		return dev_err_probe(dev, -ENODEV, "unexpected AGS2-L03 PMIC revision\n");
	key->line = devm_gpiod_get(dev, "interrupt", GPIOD_IN);
	if (IS_ERR(key->line))
		return dev_err_probe(dev, PTR_ERR(key->line), "PMIC interrupt GPIO\n");
	if (gpiod_cansleep(key->line))
		return dev_err_probe(dev, -EINVAL, "PMIC IRQ requires the AO GPIO\n");
	key->irq = gpiod_to_irq(key->line);
	if (key->irq < 0)
		return key->irq;
	key->input = devm_input_allocate_device(dev);
	if (!key->input)
		return -ENOMEM;
	key->input->name = "MediaPad T5 power key";
	key->input->phys = "ags2l03-pmic/input0";
	key->input->id.bustype = BUS_HOST;
	input_set_capability(key->input, EV_KEY, KEY_POWER);
	ret = input_register_device(key->input);
	if (ret)
		return ret;
	/*
	 * Keep this cascade live during s2idle; only the PMIC key qualifies a
	 * wake. Marking the raw GPIO as a wake IRQ would also wake on the codec.
	 * Falling-edge sensing avoids an IRQ storm on a foreign latched source.
	 * Exclusive reservation refuses takeover if another AP driver owns it.
	 */
	ret = devm_request_irq(dev, key->irq, t5_key_irq, IRQF_TRIGGER_FALLING |
			       IRQF_NO_SUSPEND | IRQF_NO_AUTOEN, dev_name(dev), key);
	if (ret)
		return dev_err_probe(dev, ret, "PMIC IRQ already owned or unavailable\n");
	ret = devm_device_init_wakeup(dev);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, key);
	ret = devm_add_action_or_reset(dev, t5_key_shutdown, key);
	if (ret)
		return ret;
	ret = t5_key_arm(key);
	if (ret)
		dev_warn(dev, "PMIC key idle check failed (%d), left disarmed\n", ret);
	else
		dev_info(dev, "PMIC power key GPIO23.7 IRQ %d armed\n", key->irq);
	return 0;
}

static void t5_key_platform_shutdown(struct platform_device *pdev)
{
	t5_key_shutdown(platform_get_drvdata(pdev));
}

static const struct of_device_id t5_key_of_match[] = {
	{ .compatible = "huawei,ags2l03-pmic-power-key" }, {}
};
MODULE_DEVICE_TABLE(of, t5_key_of_match);

static struct platform_driver t5_key_driver = {
	.probe = t5_key_probe,
	.shutdown = t5_key_platform_shutdown,
	.driver = {
		.name = "t5-pmic-power-key",
		.of_match_table = t5_key_of_match,
		.dev_groups = t5_key_groups,
		.pm = pm_sleep_ptr(&t5_key_pm),
	},
};
module_platform_driver(t5_key_driver);
MODULE_LICENSE("GPL");
