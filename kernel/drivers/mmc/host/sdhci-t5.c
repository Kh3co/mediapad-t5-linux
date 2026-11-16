// SPDX-License-Identifier: GPL-2.0-only
/* AGS2-L03 SDHCI eMMC bring-up. Immutable read-only, no write unlock.
 * PHY setup follows Huawei's GPL sdhci-of-arasan Hi6250 implementation.
 * Keep firmware supplies on; do not program PMIC/eMMC boot or WP fields.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/card.h>
#include "../core/mmc_ops.h"
#include "sdhci-pltfm.h"
#include "../core/core.h"

static bool allow_probe;
module_param(allow_probe, bool, 0600);
MODULE_PARM_DESC(allow_probe, "Enable immutable-readonly AGS2 eMMC inventory; never unlocks writes");
struct t5_emmc { struct clk *ahb, *xin; unsigned int input_hz; bool selftest_passed; unsigned int rx_phase; u32 tuning_good; };
static struct t5_emmc *t5_data(struct sdhci_host *host)
{ return sdhci_pltfm_priv(sdhci_priv(host)); }
static unsigned int t5_max_clock(struct sdhci_host *host)
{ return t5_data(host)->input_hz; }
/* AGS2 factory settings: efuse emmc_freqsel=0x12 -> DLL range 2,
 * phy-config=<0 5>, drv-strength=<4 4> (40 ohm), 200MHz xin.
 * Only controller PHY and volatile card bus/timing settings are changed. */
static unsigned int speed_mode = 3;
module_param(speed_mode, uint, 0600);
MODULE_PARM_DESC(speed_mode, "Probe profile: 0 legacy PIO, 1 HS52 ADMA, 2 HS200 ADMA, 3 HS400 ADMA");

static void t5_set_sample(struct sdhci_host *h, unsigned int phase)
{
 u32 v = sdhci_readl(h, 0x438);
 v &= ~(0x1fU << 15);
 v |= BIT(14) | BIT(13) | ((phase & 31U) << 15);
 sdhci_writel(h, v, 0x438);
 udelay(10);
 sdhci_writel(h, v & ~BIT(13), 0x438);
}

static void t5_phy_timing(struct sdhci_host *h, unsigned int hz,
                          unsigned int timing)
{
 struct t5_emmc *d = t5_data(h);
 u32 v, status, dll = 0xff, tx = 0xff;
 bool fast = timing == MMC_TIMING_MMC_HS200 || timing == MMC_TIMING_MMC_HS400;
 if (!hz) return;
 v = sdhci_readl(h, 0x430) & ~(BIT(7) | (7U << 22));
 if (h->mmc->ios.bus_mode == MMC_BUSMODE_PUSHPULL) {
  v &= ~BIT(0);
  if (timing != MMC_TIMING_LEGACY) v |= 4U << 22;
  sdhci_writel(h, 0x003ff7fc, 0x434);
 } else {
  v |= BIT(0);
  sdhci_writel(h, 0xffU << 22, 0x434);
 }
 sdhci_writel(h, v, 0x430);
 udelay(10);
 if (fast && hz >= 50000000) {
  dll = hz >= 190000000 ? 2 : hz >= 170000000 ? 0 :
        hz >= 140000000 ? 1 : hz >= 110000000 ? 2 :
        hz >= 80000000 ? 3 : 4;
  tx = timing == MMC_TIMING_MMC_HS400 ? 5 : 7;
 } else if (timing == MMC_TIMING_MMC_DDR52) {
  dll = hz >= 50000000 ? 4 : 0xff;
  tx = 4;
 }
 status = sdhci_readl(h, 0x438);
 status &= ~(BIT(8) | BIT(14) | BIT(13) | (0xfU << 9) |
             (0x1fU << 15) | (0xfU << 2) | (7U << 28));
 if (tx != 0xff) status |= BIT(8) | (tx << 9);
 if (fast && d->rx_phase < 32)
  status |= BIT(14) | (d->rx_phase << 15);
 if (dll != 0xff) status |= dll << 28;
 sdhci_writel(h, status, 0x438);
 /* The card's enhanced-strobe mode stays disabled, matching TWRP. */
 sdhci_writew(h, 0, 0x78);
 if (dll != 0xff) {
  sdhci_writel(h, v | BIT(7), 0x430);
  udelay(10);
  if (readl_poll_timeout_atomic(h->ioaddr + 0x440, status,
                               status & BIT(1), 1, 20000)) {
   WRITE_ONCE(h->mmc->t5_phy_error, true);
   dev_err(mmc_dev(h->mmc), "T5 DLL lock failed at %u Hz timing %u\n",hz,timing);
  }
 } else {
  sdhci_writel(h, v | BIT(7), 0x430);
  udelay(10);
  sdhci_writel(h, v, 0x430);
 }
}

static void t5_set_clock(struct sdhci_host *h, unsigned int hz)
{
 sdhci_set_clock(h, hz);
 t5_phy_timing(h, h->mmc->actual_clock ?: hz, h->mmc->ios.timing);
}

static void t5_set_uhs(struct sdhci_host *h, unsigned int timing)
{
 sdhci_set_uhs_signaling(h, timing);
 t5_phy_timing(h, h->mmc->actual_clock, timing);
}

static int t5_execute_tuning(struct sdhci_host *h, u32 opcode)
{
 struct t5_emmc *d = t5_data(h);
 u32 good = 0;
 unsigned int start, n, best = 0, best_len = 0, phase;
 u16 ctrl = sdhci_readw(h, SDHCI_HOST_CONTROL2);
 int ret;
 if (opcode != MMC_SEND_TUNING_BLOCK_HS200 || READ_ONCE(h->mmc->t5_phy_error)) return -EINVAL;
 /* Software phase sweep, as in Huawei's driver, comparing the complete
  * 128-byte 8-bit tuning pattern through mmc_send_tuning(). */
 ctrl &= ~(SDHCI_CTRL_EXEC_TUNING | SDHCI_CTRL_TUNED_CLK);
 sdhci_writew(h, ctrl, SDHCI_HOST_CONTROL2);
 for (phase = 0; phase < 32; phase++) {
  t5_set_sample(h, phase);
  ret = mmc_send_tuning(h->mmc, opcode, NULL);
  if (!ret) good |= BIT(phase);
 }
 for (start = 0; start < 32; start++) {
  for (n = 0; n < 32 && (good & BIT((start+n)&31)); n++);
  if (n > best_len) {best_len = n; best = (start+(n-1)/2)&31;}
 }
 d->tuning_good = good;
 if (best_len < 3) {
  d->rx_phase = 0xff;
  dev_err(mmc_dev(h->mmc),"T5 tuning no safe window: %08x\n",good);
  h->tuning_err = -EIO;
  return -EIO;
 }
 t5_set_sample(h, best);
 for (n = 0; n < 5; n++) {
  ret = mmc_send_tuning(h->mmc, opcode, NULL);
  if (ret) {h->tuning_err = ret; return ret;}
 }
 d->rx_phase = best;
 h->tuning_err = 0;
 h->mmc->retune_period = speed_mode == 2 ? 60 : 0;
 sdhci_writew(h, ctrl | SDHCI_CTRL_TUNED_CLK, SDHCI_HOST_CONTROL2);
 dev_info(mmc_dev(h->mmc),"T5 tuning PASS bitmap=%08x phase=%u window=%u/32\n",good,best,best_len);
 return 0;
}

static void t5_set_power(struct sdhci_host *host, unsigned char mode, unsigned short vdd)
{
 /* Hi6250's SDHCI interface requires POWER_180 even though NAND VCC is
  * 3.0V. Firmware owns the external rails. Do not call regulator_set_ocr. */
 sdhci_set_power_noreg(host, mode, 7);
}
static const struct sdhci_ops t5_ops = {
 .set_power = t5_set_power,
 .set_clock = t5_set_clock,
 .get_max_clock = t5_max_clock,
 .set_bus_width = sdhci_set_bus_width,
 .reset = sdhci_reset,
 .set_uhs_signaling = t5_set_uhs,
 .platform_execute_tuning = t5_execute_tuning,
};
static const struct sdhci_pltfm_data t5_pdata = {
 .ops = &t5_ops,
 .quirks = SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN | SDHCI_QUIRK_BROKEN_CARD_DETECTION,
 .quirks2 = SDHCI_QUIRK2_PRESET_VALUE_BROKEN,
};
static ssize_t readonly_status_show(struct device *dev,
 struct device_attribute *attr, char *buf)
{
 struct sdhci_host *h = platform_get_drvdata(to_platform_device(dev));
 struct mmc_host *m = h->mmc;
 return sysfs_emit(buf, "policy=%s\nunlock=content-verified-root-window\nroot_armed=%u\nroot_first=%llu\nroot_blocks=%llu\nroot_partno=%u\nmedia_ready=%u\nallowed_requests=%lld\nblocked_requests=%lld\nlast_denied_opcode=%u\nlast_denied_arg=%08x\nclock_input_hz=%u\nactual_clock_hz=%u\nbus_width=%u\ntiming=%u\ndma=%s\nphy_error=%u\ntuning_good=%08x\nrx_phase=%u\npower_control=%02x\nclock_control=%04x\npresent_state=%08x\nphy=%08x/%08x/%08x/%08x\n",
  m->t5_root_armed ? "protected-root-window" : "immutable-readonly", m->t5_root_armed, m->t5_root_first, m->t5_root_blocks, m->t5_root_partno, smp_load_acquire(&m->t5_media_ready),
  atomic64_read(&m->t5_ro_allowed), atomic64_read(&m->t5_ro_blocked),
  READ_ONCE(m->t5_ro_last_opcode), READ_ONCE(m->t5_ro_last_arg), t5_data(h)->input_hz, m->actual_clock, 1U << m->ios.bus_width, m->ios.timing, (h->flags & SDHCI_USE_ADMA) ? "ADMA" : (h->flags & SDHCI_USE_SDMA) ? "SDMA" : "PIO", READ_ONCE(m->t5_phy_error), t5_data(h)->tuning_good, t5_data(h)->rx_phase,sdhci_readb(h,SDHCI_POWER_CONTROL),sdhci_readw(h,SDHCI_CLOCK_CONTROL),sdhci_readl(h,SDHCI_PRESENT_STATE),sdhci_readl(h,0x430),sdhci_readl(h,0x434),sdhci_readl(h,0x438),sdhci_readl(h,0x440));
}
static DEVICE_ATTR_RO(readonly_status);
/* Reject an invalid SEND_STATUS argument on the native retry path. Even
 * accidental submission cannot alter card data, bus width or timing. */
static ssize_t guard_selftest_store(struct device *dev, struct device_attribute *attr,
 const char *buf, size_t count)
{
 struct sdhci_host *h = platform_get_drvdata(to_platform_device(dev));
 struct mmc_host *m = h->mmc;
 struct mmc_command cmd = { .opcode = MMC_SEND_STATUS,
  .flags = MMC_RSP_R1 | MMC_CMD_AC };
 long long before, denied; int ret; bool pass;
 if (!sysfs_streq(buf,"1")) return -EINVAL;
 mmc_claim_host(m);
 if (!m->card) {
  mmc_release_host(m); return -EAGAIN;
 }
 cmd.arg = ((u32)m->card->rca << 16) | 1U;
 before = atomic64_read(&m->t5_ro_allowed); denied = atomic64_read(&m->t5_ro_blocked);
 ret = mmc_wait_for_cmd(m,&cmd,3);
 pass = ret == -EROFS && cmd.retries == 0 &&
  atomic64_read(&m->t5_ro_allowed) == before && atomic64_read(&m->t5_ro_blocked) == denied+1;
 t5_data(h)->selftest_passed = pass;
 mmc_release_host(m);
 if (!pass) return -EIO;
 dev_info(dev,"immutable guard native retry selftest PASS; rejected before hardware submission\n");
 return count;
}
static DEVICE_ATTR_WO(guard_selftest);

static int t5_calibrate_phy(struct sdhci_host *host)
{
	u32 v = sdhci_readl(host, 0x430);
	int err;

	v = (v & ~(0xffU << 8)) | (4U << 8) | BIT(16) | BIT(25);
	sdhci_writel(host, v & ~BIT(7), 0x430);
	err = readl_poll_timeout(host->ioaddr + 0x440, v, v & BIT(6), 10, 20000);
	WRITE_ONCE(host->mmc->t5_phy_error, !!err);
	if (err)
		dev_err(mmc_dev(host->mmc), "PHY calibration timeout: %08x\n", v);
	return err;
}

static int t5_emmc_suspend(struct device *dev)
{
	struct sdhci_host *host = dev_get_drvdata(dev);
	int err = sdhci_suspend_host(host);

	if (!err)
		WRITE_ONCE(host->mmc->t5_controller_offline, true);
	return err;
}

static int t5_emmc_resume(struct device *dev)
{
	struct sdhci_host *host = dev_get_drvdata(dev);
	int err;

	/* Both controller clocks stay enabled across suspend. Restore the PHY
	 * before the card child's ordinary identification and HS200 tuning.
	 */
	err = t5_calibrate_phy(host);
	if (err)
		return err;
	err = sdhci_resume_host(host);
	if (!err)
		WRITE_ONCE(host->mmc->t5_controller_offline, false);
	return err;
}

static DEFINE_SIMPLE_DEV_PM_OPS(t5_emmc_pm, t5_emmc_suspend, t5_emmc_resume);

static int t5_emmc_probe(struct platform_device *pdev)
{
 struct sdhci_host *host; struct sdhci_pltfm_host *pltfm;
 struct t5_emmc *priv; struct mmc_host *mmc;
 void __iomem *crg; u32 cap; int ret;
 if (speed_mode > 3) return -EINVAL;
 if (!allow_probe)
  return dev_err_probe(&pdev->dev, -ENODEV, "manual immutable-RO eMMC gate is closed\n");
 host = sdhci_pltfm_init(pdev, &t5_pdata, sizeof(*priv));
 if (IS_ERR(host)) return PTR_ERR(host);
 pltfm = sdhci_priv(host); priv = t5_data(host); mmc = host->mmc;
 priv->rx_phase = 0xff;
 mmc->t5_immutable_ro = true;
 atomic64_set(&mmc->t5_ro_allowed, 0); atomic64_set(&mmc->t5_ro_blocked, 0);
 priv->ahb = devm_clk_get(&pdev->dev, "clk_ahb");
 priv->xin = devm_clk_get(&pdev->dev, "clk_xin");
 if (IS_ERR(priv->ahb) || IS_ERR(priv->xin)) {ret = -EPROBE_DEFER; goto free;}
 ret = clk_prepare_enable(priv->ahb); if (ret) goto free;
 ret = clk_prepare_enable(priv->xin); if (ret) goto ahb;
 pltfm->clk = priv->xin;
 crg = devm_ioremap(&pdev->dev, 0xfff35000, 0x1000);
 if (!crg) {ret = -ENOMEM; goto clocks;}
 /* Only the documented eMMC controller reset bit, not SD or SDIO. */
 writel(BIT(16), crg + 0x90); usleep_range(10,50);
 writel(BIT(16), crg + 0x94); usleep_range(10,50);
 cap = sdhci_readl(host, SDHCI_CAPABILITIES);
 priv->input_hz = ((cap >> 8) & 255U) * 1000000U;
 if (priv->input_hz < 10000000 || priv->input_hz > 400000000)
  priv->input_hz = 200000000; /* This board's original xin clock. */
 dev_info(&pdev->dev,"caps=%08x xin_report=%lu ahb=%lu input=%u PHY=%08x/%08x/%08x/%08x\n",
  cap,clk_get_rate(priv->xin),clk_get_rate(priv->ahb),priv->input_hz,
  sdhci_readl(host,0x430),sdhci_readl(host,0x434),sdhci_readl(host,0x438),sdhci_readl(host,0x440));
 ret = t5_calibrate_phy(host);
 if (ret) goto clocks;
 ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
 if (ret) goto clocks;
 if (!speed_mode) host->quirks |= SDHCI_QUIRK_BROKEN_DMA | SDHCI_QUIRK_BROKEN_ADMA;
 sdhci_get_of_property(pdev);
 ret = sdhci_setup_host(host); if(ret) goto clocks;
 mmc->caps &= ~(MMC_CAP_SD_HIGHSPEED | MMC_CAP_HW_RESET | MMC_CAP_UHS | MMC_CAP_BUS_WIDTH_TEST | MMC_CAP_DDR);
 mmc->caps |= MMC_CAP_NONREMOVABLE | MMC_CAP_4_BIT_DATA | MMC_CAP_8_BIT_DATA | MMC_CAP_MMC_HIGHSPEED;
 mmc->caps2 &= ~(MMC_CAP2_HS200 | MMC_CAP2_HS400 | MMC_CAP2_HS400_ES | MMC_CAP2_CQE);
 mmc->caps2 |= MMC_CAP2_NO_SD | MMC_CAP2_NO_SDIO;
 if (speed_mode >= 2) mmc->caps2 |= MMC_CAP2_HS200_1_8V_SDR;
 if (speed_mode >= 3) mmc->caps2 |= MMC_CAP2_HS400_1_8V;
 if (!speed_mode) mmc->caps &= ~(MMC_CAP_4_BIT_DATA | MMC_CAP_8_BIT_DATA | MMC_CAP_MMC_HIGHSPEED);
 mmc->f_max = !speed_mode ? 10000000 : speed_mode == 1 ? 52000000 : 200000000;
 mmc->ocr_avail = MMC_VDD_29_30 | MMC_VDD_30_31;
 ret = __sdhci_add_host(host); if(ret) {sdhci_cleanup_host(host);goto clocks;}
 ret = device_create_file(&pdev->dev,&dev_attr_readonly_status);
 if(ret) {sdhci_remove_host(host,0);goto clocks;}
 ret = device_create_file(&pdev->dev,&dev_attr_guard_selftest);
 if(ret) {device_remove_file(&pdev->dev,&dev_attr_readonly_status);sdhci_remove_host(host,0);goto clocks;}
 dev_info(&pdev->dev,"protected eMMC speed profile %u; max %u Hz; ADMA=%u; unique content root window only\n",speed_mode,mmc->f_max,!!(host->flags & SDHCI_USE_ADMA));
 return 0;
clocks: clk_disable_unprepare(priv->xin);
ahb: clk_disable_unprepare(priv->ahb);
free: return ret;
}
static void t5_emmc_remove(struct platform_device *pdev)
{
 struct sdhci_host *host=platform_get_drvdata(pdev);struct t5_emmc *d=t5_data(host);
 device_remove_file(&pdev->dev,&dev_attr_guard_selftest);
 device_remove_file(&pdev->dev,&dev_attr_readonly_status);
 sdhci_remove_host(host,0);clk_disable_unprepare(d->xin);clk_disable_unprepare(d->ahb);
}
static const struct of_device_id t5_emmc_match[] = {
 { .compatible="huawei,ags2l03-emmc-readonly" }, { }
};
MODULE_DEVICE_TABLE(of,t5_emmc_match);
static struct platform_driver t5_emmc_driver={.probe=t5_emmc_probe,.remove=t5_emmc_remove,
 .driver={.name="t5-emmc-readonly",.of_match_table=t5_emmc_match,.pm=pm_sleep_ptr(&t5_emmc_pm)}};
module_platform_driver(t5_emmc_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("AGS2-L03 eMMC immutable-readonly validation host");
