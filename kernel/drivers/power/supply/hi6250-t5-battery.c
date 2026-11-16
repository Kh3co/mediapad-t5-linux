// SPDX-License-Identifier: GPL-2.0-only
/* AGS2-L03 telemetry only. Register conversions/protocol from Huawei GPL
 * Hi6555v100 coul, HKADC and BQ2560x drivers. Assisted-by: LLM.
 * No PMIC/charger data writes, watchdog service, GPIO control or NV writes.
 */
#include <linux/delay.h>
#include <linux/mm.h>
#include <asm/memory.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/mailbox_client.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_address.h>
#include <linux/pm.h>
#include <linux/power_supply.h>
#include <linux/workqueue.h>

static bool allow_probe;
module_param(allow_probe, bool, 0600);
MODULE_PARM_DESC(allow_probe, "Allow manual AGS2-L03 read-only battery telemetry probe");

struct t5_battery {
	struct device *dev;
	struct i2c_client *charger;
	void __iomem *pmic, *tx, *rx;
	struct mbox_client mclient;
	struct mbox_chan *channel;
	u32 message[8], ntc[31];
	int va, vb, ca, cb;
	struct mutex lock;
	struct delayed_work work;
	bool paused;
	struct power_supply *battery, *usb;
	unsigned long sampled;
	int voltage, current_ua, counter, temp, adc, status, online, usb_type;
	int chip_status, chip_id, watchdog, pmic_version, checkpoint;
	int adc_error, sample_error;
 bool soc_valid;
 int counter_anchor, charge_anchor, charge_now, capacity;
};

/* RAM-only modem shutdown/reclaim experiment; charger accesses remain reads. */
int hi3660_t5_modem_sync(struct mbox_chan *chan,u32 *tx,u32 *rx);
static bool disable_modem, reclaim_modem_shared;
module_param(disable_modem,bool,0600);
module_param(reclaim_modem_shared,bool,0600);
static int modem_attempted,modem_ret=-EAGAIN;
static u32 modem_reply[8];
static unsigned long modem_freed_pages;
static void t5_shutdown_modem_and_reclaim(struct t5_battery *b)
{
 u32 tx[8]={0x00090300,0};unsigned long pfn;
 const unsigned long first=0x3ec00000UL>>PAGE_SHIFT,last=0x3f100000UL>>PAGE_SHIFT;
 if(!disable_modem)return;
 modem_attempted=1;modem_ret=hi3660_t5_modem_sync(b->channel,tx,modem_reply);
 dev_info(b->dev,"modem-off transport=%d reply=%08x/%08x\n",modem_ret,modem_reply[0],modem_reply[1]);
 if(modem_ret||modem_reply[1])return;
 if(!reclaim_modem_shared)return;
 /* DT reserves normal-mapped shared pages until after the shutdown ACK.
  * All108MiB secure/core memory remains no-map and is never released. */
 for(pfn=first;pfn<last;pfn++){
  if(!pfn_valid(pfn)||!PageReserved(pfn_to_page(pfn))){
   modem_ret=-EINVAL;dev_err(b->dev,"shared reclaim refused: invalid/nonreserved PFN%lx\n",pfn);return;
  }
 }
 modem_freed_pages=free_reserved_area(__va(0x3ec00000UL),__va(0x3f100000UL),-1,"T5 modem-shared");
 if(modem_freed_pages!=last-first)modem_ret=-EIO;
 dev_info(b->dev,"MODEM_SHARED_RECLAIM pages=%lu bytes=%lu core108MiB_kept=1\n",modem_freed_pages,modem_freed_pages*PAGE_SIZE);
}
static ssize_t modem_reclaim_state_show(struct device *dev,struct device_attribute *a,char *buf)
{
 return sysfs_emit(buf,"attempted=%d\ntransport_ret=%d\nreply0=%08x\nreply1=%08x\nfreed_pages=%lu\nfreed_bytes=%lu\n",modem_attempted,modem_ret,modem_reply[0],modem_reply[1],modem_freed_pages,modem_freed_pages*PAGE_SIZE);
}
static DEVICE_ATTR_RO(modem_reclaim_state);

static u8 t5_pmic_read(struct t5_battery *b, unsigned int reg)
{
	return readb(b->pmic + reg * 4);
}

/* Only non-destructive telemetry registers below the extended-bank boundary.
 * Retry a high-byte rollover; do not latch, calibrate or clear the counters.
 */
static u32 t5_pmic_value(struct t5_battery *b, unsigned int reg, unsigned int n)
{
	u32 value;
	u8 high;
	int retry, i;
	for (retry = 0; retry < 4; retry++) {
		high = t5_pmic_read(b, reg + n - 1);
		value = 0;
		for (i = 0; i < n; i++)
			value |= (u32)t5_pmic_read(b, reg + i) << (8 * i);
		if (high == (value >> (8 * (n - 1))))
			return value;
	}
	return value;
}

static void t5_adc_ack(struct t5_battery *b)
{
	u32 mode;
	int i;
	/* Match Huawei's RX clear/ACK handshake, on dedicated RX channel0.
	 * These are IPC message registers, not battery/charger registers.
	 */
	for (i = 0; i < 8; i++)
		writel(0, b->rx + 0x20 + i * 4);
	writel(BIT(0), b->rx + 0x18);
	mode = readl(b->rx + 0x10);
	if ((mode & BIT(6)) && !(mode & BIT(0)))
		writel(BIT(0), b->rx + 0x1c);
}

static int t5_adc_read(struct t5_battery *b)
{
	u32 mode, header, payload;
	unsigned long deadline;
	int ret;
	/* Only this diagnostic client uses RX0 in this board/kernel. Drain an
	 * old response before a new HKADC channel10 measurement request.
	 */
	if (readl(b->rx + 0x10) & BIT(6))
		t5_adc_ack(b);
	b->message[0] = 0x000b020c;
	b->message[1] = 10;
	ret = mbox_send_message(b->channel, b->message);
	if (ret < 0)
		return ret;
	ret = readl_poll_timeout(b->tx + 0x10, mode, mode & BIT(7), 100, 300000);
	mbox_client_txdone(b->channel, ret);
	if (ret)
		return ret;
	deadline = jiffies + msecs_to_jiffies(500);
	do {
		if (readl(b->rx + 0x10) & BIT(6)) {
			header = readl(b->rx + 0x20);
			payload = readl(b->rx + 0x24);
			t5_adc_ack(b);
			if (header == 0x080b020c && (payload & 0xff) == 10)
				return payload & 0xff00 ? -EIO : payload >> 16;
		}
		usleep_range(1000, 2000);
	} while (time_before(jiffies, deadline));
	return -ETIMEDOUT;
}

static int t5_ntc_temp(struct t5_battery *b, int adc)
{
	static const int degrees[] = {-273, -40, -36, -32, -28, -24, -20,
		-16, -12, -8, -4, 0, 4, 8, 12, 16, 20, 24, 28, 32, 36,
		40, 44, 48, 52, 56, 60, 64, 74, 84, 125};
	int i;
	if (adc <= 0 || adc >= 4095)
		return -ENODATA;
	if (adc >= b->ntc[1])
		return -400;
	if (adc <= b->ntc[30])
		return 1250;
	for (i = 2; i < 31; i++) {
		if (adc >= b->ntc[i])
			return degrees[i - 1] * 10 +
				(adc - (int)b->ntc[i - 1]) *
				(degrees[i] - degrees[i - 1]) * 10 /
				((int)b->ntc[i] - (int)b->ntc[i - 1]);
	}
	return -ENODATA;
}

/* AGS2-L03 scudatl OCV table from the selected stock DT. */
static const int soc_pct[] = {100,95,90,85,80,75,70,65,60,55,50,45,40,35,30,25,20,15,10,9,8,7,6,5,4,3,2,1,0};
static const int soc_temp[] = {-200,-100,0,250,400,600};
static const u16 soc_mv[][6] = {
    {4390,4388,4385,4378,4373,4366},
    {4288,4302,4312,4316,4313,4307},
    {4216,4236,4249,4257,4255,4250},
    {4153,4175,4191,4199,4197,4193},
    {4100,4116,4137,4143,4141,4137},
    {4047,4068,4086,4089,4088,4084},
    {3961,3992,4032,4040,4038,4034},
    {3920,3938,3964,3990,3990,3989},
    {3881,3899,3919,3946,3948,3947},
    {3845,3861,3879,3892,3895,3893},
    {3824,3827,3845,3856,3856,3856},
    {3807,3809,3818,3830,3831,3830},
    {3792,3796,3798,3809,3810,3809},
    {3778,3785,3786,3791,3793,3791},
    {3763,3773,3777,3776,3776,3772},
    {3749,3758,3765,3760,3754,3742},
    {3734,3739,3746,3741,3732,3719},
    {3715,3717,3719,3714,3708,3694},
    {3691,3695,3691,3690,3683,3671},
    {3684,3689,3687,3689,3682,3670},
    {3674,3683,3684,3687,3681,3668},
    {3665,3678,3680,3685,3678,3666},
    {3652,3672,3674,3680,3674,3661},
    {3635,3657,3663,3668,3662,3645},
    {3605,3630,3636,3633,3628,3608},
    {3562,3582,3587,3580,3577,3557},
    {3499,3513,3517,3509,3509,3489},
    {3398,3407,3414,3404,3409,3390},
    {3200,3200,3200,3200,3200,3200},
};

static int soc_row_mv(int row, int temp)
{
 int col;
 temp = clamp(temp, soc_temp[0], soc_temp[5]);
 for (col = 0; col < 4 && temp > soc_temp[col+1]; col++);
 return soc_mv[row][col] + (temp - soc_temp[col]) *
   ((int)soc_mv[row][col+1] - soc_mv[row][col]) /
   (soc_temp[col+1] - soc_temp[col]);
}

static int soc_from_mv(int mv, int temp)
{
 int row, high = soc_row_mv(0,temp);
 if (mv >= high) return 1000;
 for (row = 1; row < ARRAY_SIZE(soc_pct); row++) {
  int low = soc_row_mv(row,temp);
  if (mv >= low && high > low)
   return soc_pct[row]*10 + (mv-low)*(soc_pct[row-1]-soc_pct[row])*10/(high-low);
  high = low;
 }
 return 0;
}

/* Keep the gauge registers read-only. Anchor at boot and subsequently
 * integrate the signed hardware counter (charge in minus charge out). */
static void t5_estimate_charge(struct t5_battery *b)
{
 const int full_uah = 5080000; /* stock scudatl profile at 25 C */
 int temp = b->adc_error ? 250 : b->temp;
 int pmic_ocv = t5_pmic_value(b,0x196,2) & 0x7fff;
 int saved_mv = div_s64(div_s64((s64)pmic_ocv * 2400000, 10661) * b->va, 1000000) / 1000 + b->vb/1000;
 if (b->voltage < 2500000 || b->voltage > 4550000) { b->soc_valid=false; return; }
 if (!b->soc_valid) {
  int permille;
  b->counter_anchor = b->counter;
  if ((b->checkpoint & 0x80) && (b->checkpoint & 127) <= 100)
   permille = (b->checkpoint & 127) * 10;
  else if ((t5_pmic_read(b,0x195) & BIT(5)) && saved_mv >= 3000 && saved_mv <= 4450) {
   int saved_temp = (s16)t5_pmic_value(b,0x198,2);
   permille = soc_from_mv(saved_mv, clamp(saved_temp,-200,600));
   b->counter_anchor = 0; /* signed coulomb count since saved OCV */
  } else {
   /* Charging current is positive in this driver. Correct terminal
    * voltage for the stock pack's nominal 100 milliohm resistance. */
   permille = soc_from_mv(b->voltage/1000 - b->current_ua/10000,temp);
  }
  b->charge_anchor = div_s64((s64)permille*full_uah,1000);
  b->soc_valid=true;
 }
 b->charge_now = clamp_t(s64,(s64)b->charge_anchor+b->counter-b->counter_anchor,0,full_uah);
 if (b->status == POWER_SUPPLY_STATUS_FULL) {
  b->charge_now=b->charge_anchor=full_uah;
  b->counter_anchor=b->counter;
 }
 b->capacity = DIV_ROUND_CLOSEST(b->charge_now,full_uah/100);
}

static void t5_sample(struct t5_battery *b)
{
	u32 in, out, factor;
	s16 raw_current;
	int st, vbus, ret;
	s64 value;
	/* All PMIC accesses in this driver are readb(). */
	value = t5_pmic_value(b, 0x1f4, 2) & 0x7fff;
	value = div_s64(value * 2400000, 10661);
	b->voltage = div_s64(value * b->va, 1000000) + b->vb;
	raw_current = t5_pmic_value(b, 0x1f2, 2);
	value = div_s64((s64)raw_current * 5000000, 10661);
	b->current_ua = -(div_s64(value * b->ca, 1000000) + b->cb);
	in = t5_pmic_value(b, 0x1fc, 4);
	out = t5_pmic_value(b, 0x1f8, 4);
	factor = t5_pmic_read(b, 7) & BIT(2) ? 143127 : 143153;
	value = div_s64(((s64)in - out) * factor, 10000000);
	b->counter = div_s64(value * b->ca, 1000000);
	b->checkpoint = t5_pmic_read(b, 0x193);
	b->online = !!(t5_pmic_read(b, 6) & BIT(1));
	st = i2c_smbus_read_byte_data(b->charger, 8);
	b->chip_status = st;
	b->sample_error = st < 0 ? st : 0;
	b->usb_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
	b->status = POWER_SUPPLY_STATUS_UNKNOWN;
	if (st >= 0) {
		vbus = (st >> 5) & 7;
		if (vbus == 1)
			b->usb_type = POWER_SUPPLY_USB_TYPE_SDP;
		else if (vbus == 2)
			b->usb_type = POWER_SUPPLY_USB_TYPE_CDP;
		else if (vbus == 3)
			b->usb_type = POWER_SUPPLY_USB_TYPE_DCP;
		if (!b->online)
			b->status = POWER_SUPPLY_STATUS_DISCHARGING;
		else if (((st >> 3) & 3) == 3)
			b->status = POWER_SUPPLY_STATUS_FULL;
		else if (((st >> 3) & 3) == 1 || ((st >> 3) & 3) == 2)
			b->status = POWER_SUPPLY_STATUS_CHARGING;
		else
			b->status = POWER_SUPPLY_STATUS_NOT_CHARGING;
	}
	/* Read watchdog configuration for diagnostics; never reset/service it. */
	b->watchdog = i2c_smbus_read_byte_data(b->charger, 5);
	b->adc = t5_adc_read(b);
	b->adc_error = b->adc < 0 ? b->adc : 0;
	if (b->adc >= 0) {
		ret = t5_ntc_temp(b, b->adc);
		/* Negative temperatures are valid; ADC errors are tracked separately. */
		if (b->adc <= 0 || b->adc >= 4095)
			b->adc_error = ret;
		else
			b->temp = ret;
	}
	t5_estimate_charge(b);
	b->sampled = jiffies;
}

static enum power_supply_property t5_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS, POWER_SUPPLY_PROP_PRESENT, POWER_SUPPLY_PROP_CAPACITY,
 POWER_SUPPLY_PROP_CHARGE_NOW, POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_TECHNOLOGY, POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW, POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_CHARGE_COUNTER, POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
};
static enum power_supply_property t5_usb_props[] = {
	POWER_SUPPLY_PROP_ONLINE, POWER_SUPPLY_PROP_USB_TYPE,
};

static int t5_get_property(struct power_supply *psy, enum power_supply_property prop,
			  union power_supply_propval *val)
{
	struct t5_battery *b = power_supply_get_drvdata(psy);
	int ret = 0;
	mutex_lock(&b->lock);
	/* A late uevent/property read may arrive while the I2C adapter is off. */
	if (!b->paused && time_after(jiffies, b->sampled + HZ))
		t5_sample(b);
	switch (prop) {
	case POWER_SUPPLY_PROP_CAPACITY:
  if (!b->soc_valid) ret=-ENODATA; else val->intval=b->capacity; break;
 case POWER_SUPPLY_PROP_CHARGE_NOW:
  if (!b->soc_valid) ret=-ENODATA; else val->intval=b->charge_now; break;
 case POWER_SUPPLY_PROP_CHARGE_FULL: val->intval=5080000; break;
 case POWER_SUPPLY_PROP_STATUS: val->intval = b->status; break;
	case POWER_SUPPLY_PROP_PRESENT: val->intval = b->voltage > 2000000 && b->voltage < 5000000; break;
	case POWER_SUPPLY_PROP_TECHNOLOGY: val->intval = POWER_SUPPLY_TECHNOLOGY_LIPO; break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		if (b->voltage < 2000000 || b->voltage > 5000000) ret = -ENODATA;
		else val->intval = b->voltage;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW: val->intval = b->current_ua; break;
	case POWER_SUPPLY_PROP_CHARGE_COUNTER: val->intval = b->counter; break;
	case POWER_SUPPLY_PROP_TEMP:
		if (b->adc_error) ret = -ENODATA;
		else val->intval = b->temp;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN: val->intval = 5100000; break;
	case POWER_SUPPLY_PROP_ONLINE: val->intval = b->online; break;
	case POWER_SUPPLY_PROP_USB_TYPE: val->intval = b->usb_type; break;
	default: ret = -EINVAL;
	}
	mutex_unlock(&b->lock);
	return ret;
}

static const struct power_supply_desc t5_battery_desc = {
	.name = "t5-battery", .type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = t5_battery_props, .num_properties = ARRAY_SIZE(t5_battery_props),
	.get_property = t5_get_property,
};
static const struct power_supply_desc t5_usb_desc = {
	.name = "t5-usb", .type = POWER_SUPPLY_TYPE_USB,
	.properties = t5_usb_props, .num_properties = ARRAY_SIZE(t5_usb_props),
	.usb_types = BIT(POWER_SUPPLY_USB_TYPE_UNKNOWN) | BIT(POWER_SUPPLY_USB_TYPE_SDP) |
		BIT(POWER_SUPPLY_USB_TYPE_CDP) | BIT(POWER_SUPPLY_USB_TYPE_DCP),
	.get_property = t5_get_property,
};

static ssize_t telemetry_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct t5_battery *b = dev_get_drvdata(dev);
	int ret;
	mutex_lock(&b->lock);
	ret = sysfs_emit(buf,
		"charger_id=%02x\ncharger_status=%02x\nwatchdog_config=%02x\n"
		"pmic_version=%02x\nadc_raw=%d\nadc_error=%d\ni2c_error=%d\n"
		"saved_soc_checkpoint_raw=%02x\nvoltage_calibration=%d,%d\n"
		"current_calibration=%d,%d\ncontrol_writes=0\n",
		b->chip_id, b->chip_status, b->watchdog, b->pmic_version,
		b->adc, b->adc_error, b->sample_error, b->checkpoint,
		b->va, b->vb, b->ca, b->cb);
	mutex_unlock(&b->lock);
	return ret;
}
static DEVICE_ATTR_RO(telemetry);
static struct attribute *t5_attrs[] = { &dev_attr_telemetry.attr, &dev_attr_modem_reclaim_state.attr, NULL };
static const struct attribute_group t5_group = { .attrs = t5_attrs };

static void t5_work(struct work_struct *work)
{
	struct t5_battery *b = container_of(to_delayed_work(work), struct t5_battery, work);
	mutex_lock(&b->lock);
	if (b->paused) {
		mutex_unlock(&b->lock);
		return;
	}
	t5_sample(b);
	schedule_delayed_work(&b->work, msecs_to_jiffies(5000));
	mutex_unlock(&b->lock);
	power_supply_changed(b->battery);
	power_supply_changed(b->usb);
}
static void t5_free_channel(void *data) { mbox_free_channel(data); }
static void t5_cancel(void *data)
{
	struct t5_battery *b = data;
	mutex_lock(&b->lock);
	b->paused = true;
	mutex_unlock(&b->lock);
	cancel_delayed_work_sync(&b->work);
}

static int t5_battery_suspend(struct device *dev)
{
	t5_cancel(dev_get_drvdata(dev));
	return 0;
}

static int t5_battery_resume(struct device *dev)
{
	struct t5_battery *b = dev_get_drvdata(dev);

	mutex_lock(&b->lock);
	b->paused = false;
	schedule_delayed_work(&b->work, 0);
	mutex_unlock(&b->lock);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(t5_battery_pm, t5_battery_suspend,
				t5_battery_resume);

static void t5_battery_shutdown(struct i2c_client *client)
{
	t5_cancel(i2c_get_clientdata(client));
}

static int t5_battery_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct t5_battery *b;
	struct power_supply_config cfg = {};
	struct device_node *node;
	struct resource res;
	u32 calibration[4];
	int ret, i;
	if (!allow_probe)
		return dev_err_probe(dev, -ENODEV, "manual T5 telemetry gate is closed\n");
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		return -EOPNOTSUPP;
	b = devm_kzalloc(dev, sizeof(*b), GFP_KERNEL);
	if (!b) return -ENOMEM;
	b->dev = dev; b->charger = client;
	mutex_init(&b->lock);
	i2c_set_clientdata(client, b);
	b->chip_id = i2c_smbus_read_byte_data(client, 0xb);
	if (b->chip_id < 0) return b->chip_id;
	if (((b->chip_id >> 3) & 0xf) != 2) return -ENODEV;
	ret = of_property_read_u32_array(dev->of_node, "huawei,calibration", calibration, 4);
	if (ret) return ret;
	b->va = calibration[0]; b->vb = (s32)calibration[1];
	b->ca = calibration[2]; b->cb = (s32)calibration[3];
	if (b->va < 900000 || b->va > 1100000 || b->ca < 900000 || b->ca > 1100000)
		return -EINVAL;
	ret = of_property_read_u32_array(dev->of_node, "huawei,ntc-table", b->ntc, 31);
	if (ret) return ret;
	for (i = 1; i < 31; i++) if (b->ntc[i] >= b->ntc[i - 1]) return -EINVAL;
	node = of_parse_phandle(dev->of_node, "huawei,pmic", 0);
	if (!node) return -EINVAL;
	ret = of_address_to_resource(node, 0, &res); of_node_put(node);
	if (ret || res.start != 0xfff34000 || resource_size(&res) != 0x1000) return -EINVAL;
	/* PMIC remains disabled as an MFD. Shared read-only mapping; USB's
	 * existing ABB path retains its independent register ownership.
	 */
	b->pmic = devm_ioremap(dev, res.start, resource_size(&res));
	if (!b->pmic) return -ENOMEM;
	b->pmic_version = t5_pmic_read(b, 4);
	if ((b->pmic_version & 0xf) != 1) return -ENODEV;
	node = of_parse_phandle(dev->of_node, "huawei,ipc", 0);
	if (!node) return -EINVAL;
	ret = of_address_to_resource(node, 0, &res); of_node_put(node);
	if (ret || res.start != 0xe896b000 || resource_size(&res) < 17 * 0x40) return -EINVAL;
	b->rx = devm_ioremap(dev, res.start, 0x40);
	b->tx = devm_ioremap(dev, res.start + 16 * 0x40, 0x40);
	if (!b->rx || !b->tx) return -ENOMEM;
	b->mclient.dev = dev; b->mclient.knows_txdone = true;
	b->channel = mbox_request_channel(&b->mclient, 0);
	if (IS_ERR(b->channel)) return dev_err_probe(dev, PTR_ERR(b->channel), "ADC mailbox unavailable\n");
	ret = devm_add_action_or_reset(dev, t5_free_channel, b->channel);
	if (ret) return ret;
	t5_shutdown_modem_and_reclaim(b);
	t5_sample(b);
	cfg.drv_data = b; cfg.fwnode = dev_fwnode(dev);
	b->battery = devm_power_supply_register(dev, &t5_battery_desc, &cfg);
	if (IS_ERR(b->battery)) return PTR_ERR(b->battery);
	b->usb = devm_power_supply_register(dev, &t5_usb_desc, &cfg);
	if (IS_ERR(b->usb)) return PTR_ERR(b->usb);
	ret = devm_device_add_group(dev, &t5_group);
	if (ret) return ret;
	INIT_DELAYED_WORK(&b->work, t5_work);
	ret = devm_add_action_or_reset(dev, t5_cancel, b);
	if (ret) return ret;
	schedule_delayed_work(&b->work, msecs_to_jiffies(5000));
	dev_info(dev, "read-only telemetry: %duV %duA status=%d adc=%d; no charger controls\n",
		b->voltage, b->current_ua, b->status, b->adc);
	return 0;
}

static const struct of_device_id t5_battery_match[] = {
	{ .compatible = "huawei,ags2l03-battery-telemetry" }, {}
};
static struct i2c_driver t5_battery_driver = {
	.probe = t5_battery_probe,
	.shutdown = t5_battery_shutdown,
	.driver = {
		.name = "hi6250-t5-battery",
		.of_match_table = t5_battery_match,
		.pm = pm_sleep_ptr(&t5_battery_pm),
	},
};
module_i2c_driver(t5_battery_driver);
MODULE_LICENSE("GPL");
