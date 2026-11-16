// SPDX-License-Identifier: GPL-2.0-only
/* AGS2-L03 secure temperature service, matching Huawei GPL hisi_tsens.c
 * SMC 0xc5009900 and ADC calibration from the actual firmware DT.
 * Assisted-by: LLM.
 */
#include <linux/arm-smccc.h>
#include <linux/hi6250-t5.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/thermal.h>

struct t5_sensor { unsigned int id; };

static int t5_sensor_read(unsigned int id, int *temp)
{
	struct arm_smccc_res res;
	int adc;
	arm_smccc_smc(0xc5009900, id, 0, 0, 0, 0, 0, 0, &res);
	adc = (int)res.a0;
	if (adc < 116 || adc > 922) {
  pr_info_ratelimited("T5_TSENS_INVALID id=%u raw=%lx\n",id,res.a0);
  return -ENODATA;
 }
	*temp = (adc - 116) * 165000 / (922 - 116) - 40000;
	return 0;
}

/* This path does not take thermal-zone locks: governors may be stopped
 * while cpufreq policy locks are held. Thermal cooling owns emulation/QoS.
 */
int hi6250_t5_read_max_temp(int *temp)
{
	int i, value, ret, maximum = -40000;
	for (i = 0; i < 3; i++) {
		ret = t5_sensor_read(i, &value);
		if (ret)
			return ret;
		maximum = max(maximum, value);
	}
	*temp = maximum;
	return 0;
}

static int t5_temperature(struct thermal_zone_device *tz, int *temp)
{
	struct t5_sensor *sensor = thermal_zone_device_priv(tz);
	return t5_sensor_read(sensor->id, temp);
}

static const struct thermal_zone_device_ops t5_thermal_ops = {
	.get_temp = t5_temperature,
};

static int t5_thermal_probe(struct platform_device *pdev)
{
	struct t5_sensor *s;
	struct thermal_zone_device *tz;
	unsigned int i;
	s = devm_kcalloc(&pdev->dev, 3, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	for (i = 0; i < 3; i++) {
		s[i].id = i;
		tz = devm_thermal_of_zone_register(&pdev->dev, i, &s[i], &t5_thermal_ops);
		if (IS_ERR(tz))
			return dev_err_probe(&pdev->dev, PTR_ERR(tz), "sensor %u unavailable\n", i);
	}
	dev_info(&pdev->dev, "three secure temperature sensors registered\n");
	return 0;
}

static const struct of_device_id t5_thermal_match[] = {
	{ .compatible = "huawei,ags2l03-secure-tsens" }, {}
};
static struct platform_driver t5_thermal_driver = {
	.probe = t5_thermal_probe,
	.driver = { .name = "hi6250-t5-thermal", .of_match_table = t5_thermal_match },
};
builtin_platform_driver(t5_thermal_driver);
