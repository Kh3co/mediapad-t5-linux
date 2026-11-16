// SPDX-License-Identifier: GPL-2.0-only
/* AGS2-L03 inserted-card validation. Huawei GPL Hi6250 timing and board DT.
 * Controls only SD LDO16/LDO9, SD CRG reset and SD pins. No charger writes.
 */
#include <linux/clk.h>
#include <linux/gpio/consumer.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/mmc/host.h>
#include "dw_mmc.h"
#include "dw_mmc-pltfm.h"

static bool allow_probe;
module_param(allow_probe, bool, 0600);
MODULE_PARM_DESC(allow_probe, "Allow inserted-card SD-only validation probe");

static void t5_byte_bits(void __iomem *base, unsigned int reg, u32 mask, u32 val)
{
 u32 old = readl(base + reg * 4);
 writel((old & ~mask) | (val & mask), base + reg * 4);
}

static int t5_sd_init(struct dw_mci *host)
{
 unsigned long rate;
 int ret;
 ret = clk_set_rate(host->ciu_clk, 200000000);
 if (ret)
  return ret;
 rate = clk_get_rate(host->ciu_clk);
 if (!rate || rate > 400000000)
  return -EINVAL;
 /* Huawei SD SDR12 timing: external divider7, drive6, sample15. */
 writel(0, host->regs + 0x58);
 udelay(5);
 writel((15 << 16) | (6 << 21), host->regs + 0x108);
 writel(0, host->regs + 0x110);
 udelay(5);
 writel(BIT(16) | (7 << 8), host->regs + 0x58);
 host->bus_hz = rate / 8;
 dev_info(host->dev, "T5 SD input=%lu effective=%u Hz; 3V/25MHz maximum\n", rate, host->bus_hz);
 return 0;
}

static const struct dw_mci_drv_data t5_sd_data = {
 .init = t5_sd_init,
};

static int wifi74_probe(struct platform_device *pdev)
{
 struct device *dev=&pdev->dev;
 void __iomem *crg, *pads;
 int ret,i;
 if (!allow_probe) return -ENODEV;
 crg=devm_ioremap(dev,0xfff35000,0x1000);
 pads=devm_ioremap(dev,0xe896c8f4,24);
 if(!crg || !pads)return -ENOMEM;
 for(i=0;i<6;i++)writel((readl(pads+i*4)&~0xff) | (i?0x21:0x40),pads+i*4);
 writel(BIT(19),crg+0x90);usleep_range(10,50);
 writel(BIT(19),crg+0x94);usleep_range(10,50);
 ret=dma_set_mask_and_coherent(dev,DMA_BIT_MASK(32));
 if(ret)return ret;
 return dw_mci_pltfm_register(pdev,&t5_sd_data);
}
/* Called after the radio driver has powered the board. Bind just the SDIO
 * controller on its first enumeration, otherwise request a card rescan. */
void dw_mci_sdio_card_detect_change(void);
void dw_mci_sdio_card_detect_change(void)
{
 struct device *dev = bus_find_device_by_name(&platform_bus_type, NULL, "ff37d000.dwmmc2");
 struct dw_mci *host;
 int ret;
 if (!dev)
  return;
 if (!of_device_is_compatible(dev->of_node, "huawei,ags2l03-sdio-enum"))
  goto done;
 ret = device_attach(dev);
 if (ret <= 0) {
  dev_err(dev,"SDIO host bind failed: %d\n",ret);
  goto done;
 }
 device_lock(dev);
 host = dev_get_drvdata(dev);
 if (host && host->mmc)
  mmc_detect_change(host->mmc,0);
 device_unlock(dev);
done:
 put_device(dev);
}
static int t5_sd_probe(struct platform_device *pdev)
{
 void __iomem *pmic, *crg, *pads;
 int ret, i;
 if (of_device_is_compatible(pdev->dev.of_node, "huawei,ags2l03-sdio-enum")) return wifi74_probe(pdev);
 if (!allow_probe)
  return dev_err_probe(&pdev->dev, -ENODEV, "manual SD validation gate is closed\n");
 pmic = devm_ioremap(&pdev->dev, 0xfff34000, 0x1000);
 crg = devm_ioremap(&pdev->dev, 0xfff35000, 0x1000);
 pads = devm_ioremap(&pdev->dev, 0xe896c8a4, 0x18);
 if (!pmic || !crg || !pads)
  return -ENOMEM;
 ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
 if (ret)
  return ret;
 /* Read-modify-write only the SD supplies' documented fields. Power off
  * the card before returning from a previous 1.8V/UHS firmware session. */
 dev_info(&pdev->dev, "SD rails before: ldo16=%02x/%02x ldo9=%02x/%02x\n",
  readl(pmic + 0x24 * 4), readl(pmic + 0x4d * 4),
  readl(pmic + 0x1d * 4), readl(pmic + 0x46 * 4));
 t5_byte_bits(pmic, 0x24, 1, 0);
 t5_byte_bits(pmic, 0x1d, 1, 0);
 msleep(25);
 t5_byte_bits(pmic, 0x4d, 7, 7); /* LDO16 table[7] = 3.0V */
 t5_byte_bits(pmic, 0x46, 7, 6); /* LDO9 table[6] = 3.0V */
 t5_byte_bits(pmic, 0x24, 1, 1);
 usleep_range(500, 1000);
 t5_byte_bits(pmic, 0x1d, 1, 1);
 msleep(5);
 /* Board default: CLK drive0x50, CMD/DAT drive0x60 and pull-up. */
 for (i = 0; i < 6; i++) {
  u32 value = readl(pads + i * 4);
  writel((value & ~0xff) | (i ? 0x61 : 0x50), pads + i * 4);
 }
 /* Only SD reset bit18; never the eMMC bit16 or SDIO bit19. */
 writel(BIT(18), crg + 0x90);
 usleep_range(10, 50);
 writel(BIT(18), crg + 0x94);
 usleep_range(10, 50);
 return dw_mci_pltfm_register(pdev, &t5_sd_data);
}

static const struct of_device_id t5_sd_match[] = {
 { .compatible = "huawei,ags2l03-sdio-enum" }, { .compatible = "huawei,ags2l03-sd-validation" }, { }
};
MODULE_DEVICE_TABLE(of, t5_sd_match);
static struct platform_driver t5_sd_driver = {
 .probe = t5_sd_probe,
 .remove = dw_mci_pltfm_remove,
 .driver = { .name = "t5-sd-validation", .of_match_table = t5_sd_match },
};
module_platform_driver(t5_sd_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("AGS2-L03 conservative SD validation glue");
