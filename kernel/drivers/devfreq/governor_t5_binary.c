// SPDX-License-Identifier: GPL-2.0-only
/* AGS2-L03 two-endpoint GPU policy. Assisted-by: LLM. */
#include <linux/devfreq.h>
#include <linux/devfreq-governor.h>
#include <linux/hi6250-t5.h>
#include <linux/module.h>
#include <linux/of.h>

static int t5_gpu_target(struct devfreq *df, unsigned long *freq)
{
	struct hi6250_t5_gpu_policy *policy = df->data;
	int maximum=0, ret;
 bool was_hot=policy->hot;

	ret = devfreq_update_stats(df);
	if (ret)
		return ret;
	/* No thermal-zone locks while holding the devfreq lock. Thermal QoS
	 * separately covers emulated temperatures and all three DT zones.
	 */
	ret = hi6250_t5_read_max_temp(&maximum);
	if (ret || maximum >= 90000)
		policy->hot = true;
	else if (maximum <= 85000)
		policy->hot = false;
	if(was_hot!=policy->hot)dev_info(df->dev.parent,"T5_GPU_THERM old=%u new=%u temp_ret=%d temp=%d busy=%lu total=%lu\n",was_hot,policy->hot,ret,maximum,df->last_status.busy_time,df->last_status.total_time);
	*freq = !policy->hot && df->last_status.busy_time ?
		DEVFREQ_MAX_FREQ : DEVFREQ_MIN_FREQ;
	return 0;
}

static int t5_gpu_event(struct devfreq *df, unsigned int event, void *data)
{
	switch (event) {
	case DEVFREQ_GOV_START:
		if (!of_device_is_compatible(df->dev.parent->of_node,
					    "huawei,ags2l03-mali-t830") || !df->data)
			return -ENODEV;
		devfreq_monitor_start(df);
		break;
	case DEVFREQ_GOV_STOP:
		devfreq_monitor_stop(df);
		break;
	case DEVFREQ_GOV_UPDATE_INTERVAL:
		devfreq_update_interval(df, data);
		break;
	case DEVFREQ_GOV_SUSPEND:
		devfreq_monitor_suspend(df);
		break;
	case DEVFREQ_GOV_RESUME:
		devfreq_monitor_resume(df);
		break;
	}
	return 0;
}

static struct devfreq_governor t5_gpu_governor = {
	.name = "t5_binary",
	/* Keep this board's policy and temperature guard in force. */
	.flags = DEVFREQ_GOV_FLAG_IMMUTABLE,
	.get_target_freq = t5_gpu_target,
	.event_handler = t5_gpu_event,
};

static int __init t5_gpu_governor_init(void)
{
	return devfreq_add_governor(&t5_gpu_governor);
}
subsys_initcall(t5_gpu_governor_init);
MODULE_LICENSE("GPL");
