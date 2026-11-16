/* SPDX-License-Identifier: GPL-2.0-only */
/* Local bring-up diagnostics and probe gate. Assisted-by: LLM. */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include "panfrost_regs.h"

static bool t5_allow_probe;
module_param(t5_allow_probe, bool, 0600);
MODULE_PARM_DESC(t5_allow_probe, "Allow manual AGS2-L03 GPU bring-up probe");

static bool panfrost_t5_device(struct device *dev)
{
	return of_device_is_compatible(dev->of_node, "huawei,ags2l03-mali-t830");
}

static ssize_t t5_power_status_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct panfrost_device *pfdev = dev_get_drvdata(dev);
	unsigned long long before, after;
 u64 t0,t1,ns,hz;
	int ret;
	if (!panfrost_t5_device(dev))
		return -EOPNOTSUPP;
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		return ret;
	panfrost_cycle_counter_get(pfdev);
	t0=ktime_get_ns();
 before = panfrost_cycle_counter_read(pfdev);
	usleep_range(10000,10500);
	after = panfrost_cycle_counter_read(pfdev);
 t1=ktime_get_ns();ns=t1-t0;hz=ns?div64_u64((after-before)*1000000000ULL,ns):0;
	panfrost_cycle_counter_put(pfdev);
	ret = sysfs_emit(buf,
		"gpu_id=%08x\nclock_hz=%lu\nshader_present=%llx\nshader_ready=%08x\n"
		"tiler_present=%llx\ntiler_ready=%08x\nl2_present=%llx\nl2_ready=%08x\n"
		"cycles_before=%llu\ncycles_after=%llu\ncycle_elapsed_ns=%llu\nmeasured_hz=%llu\n",
		readl(pfdev->iomem + GPU_ID), clk_get_rate(pfdev->clock),
		(unsigned long long)pfdev->features.shader_present,
		readl(pfdev->iomem + SHADER_READY_LO),
		(unsigned long long)pfdev->features.tiler_present,
		readl(pfdev->iomem + TILER_READY_LO),
		(unsigned long long)pfdev->features.l2_present,
		readl(pfdev->iomem + L2_READY_LO), before, after,ns,hz);
	pm_runtime_put(dev);
	return ret;
}
static DEVICE_ATTR_RO(t5_power_status);
