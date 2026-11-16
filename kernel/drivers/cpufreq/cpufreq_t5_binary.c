// SPDX-License-Identifier: GPL-2.0-only
/* T5 experimental two-speed governor. Assisted-by: LLM.
 * All clusters boost together above 1% utilization on any online CPU.
 * 20ms samples; no middle frequency requests. Invalid sensors fail low.
 */
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/hi6250-t5.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/thermal.h>
#include <linux/workqueue.h>

struct t5_policy {
	struct cpufreq_policy *policy;
	struct delayed_work work;
	u64 idle[NR_CPUS], wall[NR_CPUS];
	bool running, hot;
};

static bool t5_temperature_ok(struct t5_policy *d)
{
	int maximum;
	if (hi6250_t5_read_max_temp(&maximum))
		return false;
	if (maximum >= 90000)
		d->hot = true;
	else if (maximum <= 85000)
		d->hot = false;
	return !d->hot;
}

static void t5_sample(struct work_struct *work)
{
	struct t5_policy *d = container_of(to_delayed_work(work), struct t5_policy, work);
	struct cpufreq_policy *p = d->policy;
	bool busy = false, cool;
	unsigned int cpu, target;
	int ret;
	if (!READ_ONCE(d->running))
		return;
	cool = t5_temperature_ok(d);
	/* Avoid waiting on locks held by stop/hotplug while they drain work. */
	if (!cpus_read_trylock())
		goto again;
	if (!down_read_trylock(&p->rwsem)) {
		cpus_read_unlock();
		goto again;
	}
	for_each_online_cpu(cpu) {
		u64 wall = 0, idle = get_cpu_idle_time(cpu, &wall, 1);
		u64 dw = wall - d->wall[cpu], di = idle - d->idle[cpu];
		if (d->wall[cpu] && idle != -1ULL && dw && di <= dw &&
		    (dw - di) * 100 > dw)
			busy = true;
		d->wall[cpu] = wall;
		d->idle[cpu] = idle;
	}
	target = busy && cool ? p->max : p->min;
	ret = __cpufreq_driver_target(p, target, CPUFREQ_RELATION_L);
	if (ret)
		pr_warn_ratelimited("t5_binary: cpu%u target %u failed: %d\n", p->cpu, target, ret);
	up_read(&p->rwsem);
	cpus_read_unlock();
again:
	if (READ_ONCE(d->running))
		queue_delayed_work(system_freezable_wq, &d->work, msecs_to_jiffies(20));
}

static int t5_init(struct cpufreq_policy *p)
{
	struct t5_policy *d = kzalloc(sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	d->policy = p;
	INIT_DELAYED_WORK(&d->work, t5_sample);
	p->governor_data = d;
	return 0;
}

static void t5_exit(struct cpufreq_policy *p)
{
	kfree(p->governor_data);
	p->governor_data = NULL;
}

static int t5_start(struct cpufreq_policy *p)
{
	struct t5_policy *d = p->governor_data;
	memset(d->wall, 0, sizeof(d->wall));
	memset(d->idle, 0, sizeof(d->idle));
	WRITE_ONCE(d->running, true);
	queue_delayed_work(system_freezable_wq, &d->work, msecs_to_jiffies(20));
	return 0;
}

static void t5_stop(struct cpufreq_policy *p)
{
	struct t5_policy *d = p->governor_data;
	WRITE_ONCE(d->running, false);
	cancel_delayed_work_sync(&d->work);
}

static void t5_limits(struct cpufreq_policy *p)
{
	__cpufreq_driver_target(p, p->cur, CPUFREQ_RELATION_L);
}

static struct cpufreq_governor t5_governor = {
	.name = "t5_binary", .init = t5_init, .exit = t5_exit,
	.start = t5_start, .stop = t5_stop, .limits = t5_limits,
	.owner = THIS_MODULE, .flags = CPUFREQ_GOV_DYNAMIC_SWITCHING,
};
cpufreq_governor_init(t5_governor);
cpufreq_governor_exit(t5_governor);
MODULE_LICENSE("GPL");
