// SPDX-License-Identifier: GPL-2.0-only
/* AGS2-L03 AP-owned SoC RTC alarm. The battery-backed PMIC RTC is separate. */
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeirq.h>
#include <linux/rtc.h>

#define T5_RTC_DR	0x00
#define T5_RTC_MR	0x04
#define T5_RTC_CR	0x0c
#define T5_RTC_IMSC	0x10
#define T5_RTC_RIS	0x14
#define T5_RTC_MIS	0x18
#define T5_RTC_ICR	0x1c
#define T5_ALARM	BIT(0)

static bool allow_probe;
module_param(allow_probe, bool, 0600);
MODULE_PARM_DESC(allow_probe, "Allow a subsequent manual platform bind for RTC alarm validation");
static bool inspect_only = true;
module_param(inspect_only, bool, 0600);
MODULE_PARM_DESC(inspect_only, "Read the inherited SoC RTC twice without claiming an IRQ or registering an RTC");

struct t5_rtc {
	void __iomem *base;
	struct rtc_device *rtc;
	spinlock_t lock;
	bool owned;
	u32 original_match;
	u32 target;
};

static bool t5_existing_alarm(struct t5_rtc *r)
{
	return readl(r->base + T5_RTC_IMSC) || readl(r->base + T5_RTC_RIS) ||
	       readl(r->base + T5_RTC_MIS);
}

/* Called under lock. Only change a match value we installed ourselves. */
static int t5_release_alarm(struct t5_rtc *r)
{
	u32 mask;

	if (!r->owned)
		return 0;
	if (readl(r->base + T5_RTC_MR) != r->target) {
		r->owned = false;
		return -EBUSY;
	}

	mask = readl(r->base + T5_RTC_IMSC);
	writel(mask & ~T5_ALARM, r->base + T5_RTC_IMSC);
	writel(T5_ALARM, r->base + T5_RTC_ICR);
	writel(r->original_match, r->base + T5_RTC_MR);
	readl(r->base + T5_RTC_MR);
	r->owned = false;
	return 0;
}

static int t5_read_time(struct device *dev, struct rtc_time *time)
{
	struct t5_rtc *r = dev_get_drvdata(dev);

	rtc_time64_to_tm(readl(r->base + T5_RTC_DR), time);
	return 0;
}

static int t5_read_alarm(struct device *dev, struct rtc_wkalrm *alarm)
{
	struct t5_rtc *r = dev_get_drvdata(dev);
	unsigned long flags;

	spin_lock_irqsave(&r->lock, flags);
	rtc_time64_to_tm(readl(r->base + T5_RTC_MR), &alarm->time);
	alarm->enabled = !!(readl(r->base + T5_RTC_IMSC) & T5_ALARM);
	alarm->pending = !!(readl(r->base + T5_RTC_RIS) & T5_ALARM);
	spin_unlock_irqrestore(&r->lock, flags);
	return 0;
}

static int t5_set_alarm(struct device *dev, struct rtc_wkalrm *alarm)
{
	struct t5_rtc *r = dev_get_drvdata(dev);
	time64_t target = rtc_tm_to_time64(&alarm->time);
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&r->lock, flags);
	ret = t5_release_alarm(r);
	if (ret || !alarm->enabled)
		goto done;
	if (t5_existing_alarm(r)) {
		ret = -EBUSY;
		goto done;
	}
	if (target <= readl(r->base + T5_RTC_DR) || target > U32_MAX) {
		ret = -ETIME;
		goto done;
	}
	r->original_match = readl(r->base + T5_RTC_MR);
	r->target = target;
	r->owned = true;
	writel(r->target, r->base + T5_RTC_MR);
	writel(readl(r->base + T5_RTC_IMSC) | T5_ALARM, r->base + T5_RTC_IMSC);
	if (readl(r->base + T5_RTC_MR) != r->target ||
	    !(readl(r->base + T5_RTC_IMSC) & T5_ALARM)) {
		t5_release_alarm(r);
		ret = -EIO;
	}
done:
	spin_unlock_irqrestore(&r->lock, flags);
	return ret;
}

static int t5_alarm_irq_enable(struct device *dev, unsigned int enable)
{
	struct t5_rtc *r = dev_get_drvdata(dev);
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&r->lock, flags);
	if (enable)
		ret = r->owned && readl(r->base + T5_RTC_MR) == r->target &&
		      (readl(r->base + T5_RTC_IMSC) & T5_ALARM) ? 0 : -EBUSY;
	else
		ret = t5_release_alarm(r);
	spin_unlock_irqrestore(&r->lock, flags);
	return ret;
}

static const struct rtc_class_ops t5_rtc_ops = {
	.read_time = t5_read_time,
	.read_alarm = t5_read_alarm,
	.set_alarm = t5_set_alarm,
	.alarm_irq_enable = t5_alarm_irq_enable,
};

static irqreturn_t t5_rtc_interrupt(int irq, void *arg)
{
	struct t5_rtc *r = arg;
	unsigned long flags;
	bool fired = false;

	spin_lock_irqsave(&r->lock, flags);
	if (r->owned && (readl(r->base + T5_RTC_MIS) & T5_ALARM))
		fired = !t5_release_alarm(r);
	spin_unlock_irqrestore(&r->lock, flags);
	if (!fired)
		return IRQ_NONE;
	rtc_update_irq(r->rtc, 1, RTC_IRQF | RTC_AF);
	return IRQ_HANDLED;
}

static void t5_cleanup(void *arg)
{
	struct t5_rtc *r = arg;
	unsigned long flags;

	spin_lock_irqsave(&r->lock, flags);
	t5_release_alarm(r);
	spin_unlock_irqrestore(&r->lock, flags);
}

struct t5_rtc_snapshot {
	u32 cr, imsc, ris, mis, mr, dr;
};

/* All six registers have side-effect-free reads; ICR is never read. */
static struct t5_rtc_snapshot t5_snapshot(struct device *dev, struct t5_rtc *r,
					   const char *label)
{
	struct t5_rtc_snapshot s;

	s.cr = readl(r->base + T5_RTC_CR);
	s.imsc = readl(r->base + T5_RTC_IMSC);
	s.ris = readl(r->base + T5_RTC_RIS);
	s.mis = readl(r->base + T5_RTC_MIS);
	s.mr = readl(r->base + T5_RTC_MR);
	s.dr = readl(r->base + T5_RTC_DR);
	dev_info(dev, "T5_RTC_SNAPSHOT %s CR=%08x IMSC=%08x RIS=%08x MIS=%08x MR=%08x DR=%08x\n",
		 label, s.cr, s.imsc, s.ris, s.mis, s.mr, s.dr);
	return s;
}

static int t5_rtc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct t5_rtc *r;
	struct clk *pclk;
	struct t5_rtc_snapshot first, last;
	int irq, ret;

	if (!READ_ONCE(allow_probe))
		return -ENODEV;
	pclk = devm_clk_get(dev, NULL);
	if (IS_ERR(pclk))
		return dev_err_probe(dev, PTR_ERR(pclk), "RTC bus clock\n");
	if (!__clk_is_enabled(pclk))
		return dev_err_probe(dev, -ENODEV, "firmware RTC bus clock inactive\n");
	/* The validation DT keeps inherited clocks on. No one else uses the
	 * dedicated RTC APB gate. Leave it untouched, including on failure.
	 */
	r = devm_kzalloc(dev, sizeof(*r), GFP_KERNEL);
	if (!r)
		return -ENOMEM;
	r->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(r->base))
		return PTR_ERR(r->base);
	first = t5_snapshot(dev, r, "initial");
	msleep(1200);
	if (!__clk_is_enabled(pclk))
		return dev_err_probe(dev, -ENODEV, "RTC clock went inactive during probe\n");
	last = t5_snapshot(dev, r, "later");
	if (READ_ONCE(inspect_only))
		return dev_err_probe(dev, -ENODEV, "T5_RTC_INSPECT_ONLY_DONE; no IRQ claimed\n");
	if (!(first.cr & BIT(0)) || !(last.cr & BIT(0)))
		return dev_err_probe(dev, -ENODEV, "firmware RTC counter disabled\n");
	if (first.imsc || first.ris || first.mis || last.imsc || last.ris || last.mis ||
	    first.mr != last.mr)
		return dev_err_probe(dev, -EBUSY, "inherited RTC alarm state; leaving it alone\n");
	if (last.dr == first.dr || last.dr - first.dr > 3)
		return dev_err_probe(dev, -ENODEV, "firmware RTC did not advance normally\n");
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	spin_lock_init(&r->lock);
	ret = devm_request_irq(dev, irq, t5_rtc_interrupt, IRQF_NO_AUTOEN,
			       "t5-soc-rtc", r);
	if (ret)
		return dev_err_probe(dev, ret, "exclusive alarm IRQ unavailable\n");
	platform_set_drvdata(pdev, r);
	r->rtc = devm_rtc_allocate_device(dev);
	if (IS_ERR(r->rtc))
		return PTR_ERR(r->rtc);
	r->rtc->ops = &t5_rtc_ops;
	r->rtc->range_max = U32_MAX;
	ret = devm_device_init_wakeup(dev);
	if (ret)
		return ret;
	ret = devm_pm_set_wake_irq(dev, irq);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, t5_cleanup, r);
	if (ret)
		return ret;
	if (t5_existing_alarm(r))
		return dev_err_probe(dev, -EBUSY, "alarm changed during probe\n");
	enable_irq(irq);
	return devm_rtc_register_device(r->rtc);
}

static void t5_rtc_shutdown(struct platform_device *pdev)
{
	t5_cleanup(platform_get_drvdata(pdev));
}

static const struct of_device_id t5_rtc_match[] = {
	{ .compatible = "huawei,ags2-l03-soc-rtc-alarm" },
	{}
};
MODULE_DEVICE_TABLE(of, t5_rtc_match);

static struct platform_driver t5_rtc_driver = {
	.probe = t5_rtc_probe,
	.shutdown = t5_rtc_shutdown,
	.driver = { .name = "t5-soc-rtc-alarm", .of_match_table = t5_rtc_match },
};
module_platform_driver(t5_rtc_driver);
MODULE_LICENSE("GPL");
