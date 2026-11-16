// SPDX-License-Identifier: GPL-2.0-only
/* AGS2-L03: the boot firmware configures the LED strings, bias and boost.
 * Linux owns only the two brightness fields of the selected BOE panel.
 */
#include <linux/backlight.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>

#define T5_BL_LOW  0x10
#define T5_BL_HIGH 0x11
#define T5_MAX_LEVEL 4095
#define T5_BOOT_LEVEL 0x266 /* stock panel's initial brightness register pair */

static int t5_read_level(struct i2c_client *chip, int *high_byte)
{
	int lo = i2c_smbus_read_byte_data(chip, T5_BL_LOW);
	int hi;

	if (lo < 0)
		return lo;
	hi = i2c_smbus_read_byte_data(chip, T5_BL_HIGH);
	if (hi < 0)
		return hi;
	if (high_byte)
		*high_byte = hi;
	return ((hi & 0x0f) << 8) | lo;
}

static int t5_bl_get(struct backlight_device *bl)
{
	return t5_read_level(bl_get_data(bl), NULL);
}

static int t5_bl_update(struct backlight_device *bl)
{
	struct i2c_client *chip = bl_get_data(bl);
	int level = backlight_get_brightness(bl), old_hi, old, ret;

	old = t5_read_level(chip, &old_hi);
	if (old < 0 || old == level)
		return old < 0 ? old : 0;
	ret = i2c_smbus_write_byte_data(chip, T5_BL_LOW, level & 0xff);
	if (!ret)
		ret = i2c_smbus_write_byte_data(chip, T5_BL_HIGH,
					      (old_hi & 0xf0) | (level >> 8));
	if (!ret) {
		ret = t5_read_level(chip, NULL);
		if (ret == level)
			return 0;
		if (ret >= 0)
			ret = -EIO;
	}
	/* Restore both bytes after an interrupted write, including a lost ACK. */
	i2c_smbus_write_byte_data(chip, T5_BL_LOW, old & 0xff);
	i2c_smbus_write_byte_data(chip, T5_BL_HIGH, old_hi);
	if (t5_read_level(chip, NULL) != old)
		dev_err_ratelimited(&chip->dev, "could not restore brightness %d\n", old);
	return ret;
}

static const struct backlight_ops t5_bl_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.get_brightness = t5_bl_get,
	.update_status = t5_bl_update,
};

static int t5_bl_probe(struct i2c_client *chip)
{
	struct backlight_properties props = {
		.type = BACKLIGHT_RAW, .max_brightness = T5_MAX_LEVEL,
		.scale = BACKLIGHT_SCALE_LINEAR,
	};
	struct backlight_device *bl;
	int level;

	if (!of_machine_is_compatible("huawei,agassi2") || chip->addr != 0x2c)
		return -ENODEV;
	if (!i2c_check_functionality(chip->adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		return -EOPNOTSUPP;
	level = t5_read_level(chip, NULL);
	if (level < 0)
		return dev_err_probe(&chip->dev, level, "brightness registers\n");
	props.brightness = level ? level : T5_BOOT_LEVEL;
	props.power = level ? BACKLIGHT_POWER_ON : BACKLIGHT_POWER_OFF;
	bl = devm_backlight_device_register(&chip->dev, "t5-lp8556", &chip->dev,
					    chip, &t5_bl_ops, &props);
	if (IS_ERR(bl))
		return PTR_ERR(bl);
	dev_info(&chip->dev, "firmware brightness %d/4095 inherited\n", level);
	return 0;
}

static const struct of_device_id t5_bl_ids[] = {
	{ .compatible = "huawei,ags2-l03-lp8556" }, {}
};
MODULE_DEVICE_TABLE(of, t5_bl_ids);
static struct i2c_driver t5_bl_driver = {
	.driver = { .name = "t5-lp8556", .of_match_table = t5_bl_ids },
	.probe = t5_bl_probe,
};
module_i2c_driver(t5_bl_driver);
MODULE_LICENSE("GPL");
