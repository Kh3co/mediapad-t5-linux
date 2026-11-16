// SPDX-License-Identifier: GPL-2.0-only
#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/regulator/consumer.h>
#include <linux/gpio/consumer.h>
#include <linux/moduleparam.h>
#include <linux/firmware.h>
#include <linux/vmalloc.h>
#include <linux/crc32.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/timer.h>
#include <linux/math64.h>
#include <linux/wait.h>
#include <linux/sizes.h>
#include <linux/mailbox_client.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define HIFI_MB_BASE 0x35212c00
#define RING_BYTES 0x1800
#define DATA_WORDS ((RING_BYTES - 16) / 4)
#define AUDIO_CODE 0x00040003

#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/unaligned.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

#define T5_FRAMES 960U
#define T5_PERIOD_BYTES (T5_FRAMES * 4)
#define T5_BUF_BYTES (2 * T5_PERIOD_BYTES)

struct t5_board {
    struct t5_rx_irq *rx;
    struct snd_card *card;
    struct snd_pcm *pcm;
    struct t5_stream *stream;
    bool unavailable;
    bool mailbox_init_attempted;
    struct t5_bus *bus;
    struct mutex resources, transmit;
    u32 sequence;
};
static struct t5_rx_irq *t5_rx(struct device *dev)
{
    struct t5_board *b = dev_get_drvdata(dev);
    return b->rx;
}
struct t5_event {
    u32 mode, op, ticks, header_ticks;
    u64 rx_ns;
};
struct t5_events {
    struct t5_event data[32];
    unsigned int head, tail;
};
struct t5_rx_irq {
    struct mutex dispatch;
    struct t5_events msgs[2];
    int broken;
	void __iomem *chan;
	void __iomem *slice;
	wait_queue_head_t pending;
	bool wait_on_irq;
	u32 max_rx_age_ticks;
	atomic_t count;
	u32 first_mode, first_src;
	int irq;
	
};

struct timing_sample {
	u32 dsp_ticks, header_ticks, sequence;
	u64 rx_ns, fill_ns, publish_ns, consumed_ns;
};
/* Dedicated stock HiFi -> AP mailbox 2, SPI 199, destination GIC_1. */
static irqreturn_t hifi_doorbell(int irq,void *opaque)
{
	struct t5_rx_irq *ctx=opaque;
	void __iomem *b=ctx->chan;
	u32 mode=readl(b+0x10), mask=readl(b+0x14), src=readl(b);
	if (!(mode&BIT(6)) || (mask&BIT(0)) || !(src&BIT(4)))
		return IRQ_NONE;
	int event = atomic_inc_return(&ctx->count);
	if (event == 1) {ctx->first_mode=mode;ctx->first_src=src;}
	for (int i=0;i<8;i++)writel(0,b+0x20+i*4);
	writel(BIT(0),b+0x18);
	if (!(mode&BIT(0)))writel(BIT(0),b+0x1c);
	wake_up(&ctx->pending);
	return IRQ_HANDLED;
}
#define NV_LEN 0x4b968
#define NV_ADDR (HIFI_MB_BASE-0x4bc00)
static void write_nv(void __iomem *area, const u8 *data)
{
	writel(0,area);
	memcpy_toio(area+4,data+4,NV_LEN-4);
	wmb();
	memcpy_toio(area,data,4);
	wmb();
}
static const bool lower_output; /* Verified stock HSL/R -4.5 dB */
struct t5_peri_vote {
	struct device *dev;
	struct regulator *supply;
};

static void t5_peri_release(void *data)
{
	struct t5_peri_vote *v = data;
	int ret = regulator_disable(v->supply);

	if (ret)
		dev_err(v->dev, "sys_peri release failed %d; vote state unconfirmed\n", ret);
	else
		dev_info(v->dev, "sys_peri vote released\n");
}

static int t5_peri_acquire(struct device *dev)
{
	struct t5_peri_vote *v = devm_kzalloc(dev, sizeof(*v), GFP_KERNEL);
	int ret;

	if (!v)
		return -ENOMEM;
	v->dev = dev;
	/* Refuse a dummy supply; the rail is optional only for baseline runs. */
	v->supply = devm_regulator_get_optional(dev, "sys-peri");
	if (IS_ERR(v->supply))
		return dev_err_probe(dev, PTR_ERR(v->supply), "sys_peri provider\n");
	ret = regulator_enable(v->supply);
	if (ret)
		return dev_err_probe(dev, ret, "sys_peri enable\n");
	/* Added before acquisition of codec clock resources; reverse-order
	 * devres teardown drops the clocks before releasing the rail. */
	ret = devm_add_action_or_reset(dev, t5_peri_release, v);
	if (!ret)
		dev_info(dev, "sys_peri vote granted before codec clocks\n");
	return ret;
}

struct pmu_step { u16 reg; u8 mask, value, delay_ms; bool output_gain; };
static const struct pmu_step path[] = {
 {0x260, 0x01, 0x01, 0}, /* dacl */
 {0x28f, 0x02, 0x02, 0}, /* dacl */
 {0, 0, 0, 1},
 {0x260, 0x02, 0x02, 0}, /* dacr */
 {0x28f, 0x01, 0x01, 0}, /* dacr */
 {0, 0, 0, 1},
 {0x27e, 0x13, 0x13, 0}, /* hs_preprocess */
 {0x27a, 0xff, 0x21, 0}, /* hs_preprocess */
 {0x27b, 0xff, 0x21, 0}, /* hs_preprocess */
 {0x282, 0x80, 0x80, 0}, /* hs_preprocess */
 {0x279, 0x80, 0x80, 0}, /* hs_preprocess */
 {0x282, 0x80, 0x00, 0}, /* hs_preprocess */
 {0x27e, 0x08, 0x08, 0}, /* hs_preprocess */
 {0x27d, 0xff, 0xfa, 0}, /* hs_preprocess */
 {0x285, 0xff, 0x00, 0}, /* hs_preprocess */
 {0x282, 0x08, 0x00, 0}, /* hs_preprocess */
 {0x28e, 0x02, 0x02, 0}, /* hs_preprocess */
 {0, 0, 0, 2},
 {0x276, 0x84, 0x84, 0}, /* dac_path */
 {0x26c, 0x30, 0x00, 0}, /* smt_hslr_pd_mixer */
 {0x27a, 0xff, 0x58, 0, true}, /* smt_hslr_pd_outdrv */
 {0x27b, 0xff, 0x58, 0, true}, /* smt_hslr_pd_outdrv */
 {0x26c, 0x06, 0x00, 0}, /* smt_hslr_pd_outdrv */
 {0, 0, 0, 100},
 {0x27a, 0xff, 0x58, 0, true}, /* speaker gain */
 {0x27b, 0xff, 0x58, 0, true}, /* speaker gain */
};
/* The stock PMU uses word transactions at four-byte spacing. */
static int configure_analog(struct device *dev, void __iomem *pmu,
			    u8 *saved, int *applied)
{
	const u32 gain_code = READ_ONCE(lower_output) ? 6 : 12;
	const u32 gain_mask = GENMASK(4, 1);
	u32 hsl, hsr;

	for (int i = 0; i < ARRAY_SIZE(path); i++) {
		const struct pmu_step *step = &path[i];
		void __iomem *reg = pmu + step->reg * 4;
		u32 value = step->value;
		u32 old;

		if (step->delay_ms) {
			msleep(step->delay_ms);
			continue;
		}
		if (step->output_gain)
			value = (value & ~gain_mask) | (gain_code << 1);
		old = readl(reg);
		saved[i] = old;
		writel((old & ~step->mask) | value, reg);
		*applied = i + 1;
		if ((readl(reg) & step->mask) != value) {
			dev_err(dev, "PMU readback failed for reg %03x mask %02x\n",
				step->reg, step->mask);
			return -EIO;
		}
	}
	hsl = readl(pmu + 0x27a * 4) & 0xff;
	hsr = readl(pmu + 0x27b * 4) & 0xff;
	if (hsl != (BIT(6) | (gain_code << 1)) || hsr != hsl) {
		dev_err(dev, "HSL/R gain readback mismatch code=%u actual=%02x/%02x\n",
			gain_code, hsl, hsr);
		return -EIO;
	}
	dev_info(dev, "HSL/R gain code=%u target=%s dB registers=%02x/%02x verified\n",
		 gain_code, gain_code == 6 ? "-10.5" : "-4.5", hsl, hsr);
	return 0;
}

static int restore_analog(void __iomem *pmu, const u8 *saved, int applied)
{
	int ret = 0;

	while (applied) {
		int i = --applied;
		const struct pmu_step *step = &path[i];
		void __iomem *reg = pmu + step->reg * 4;

		if (step->delay_ms) {
			msleep(step->delay_ms);
			continue;
		}
		writel((readl(reg) & ~step->mask) | (saved[i] & step->mask), reg);
		if ((readl(reg) & step->mask) != (saved[i] & step->mask))
			ret = -EIO;
	}
	return ret;
}
struct digital_step { u8 bank; u16 offset; u32 mask, value; };
static const struct digital_step digital[] = {
 {0,0xc,0x00070000,0x00040000}, /* HISSC_FS_DACLR */
 {0,0x50,0x00000080,0x00000080}, /* HISSC_DACL_AGC_BYPASS */
 {0,0x58,0x00000080,0x00000080}, /* HISSC_DACR_AGC_BYPASS */
 {0,0xc,0x00000070,0x00000040}, /* HISSC_FS_STEREO_DLINK_L */
 {0,0xc,0x00000700,0x00000400}, /* HISSC_FS_STEREO_DLINK_R */
 {0,0x28,0x000000ff,0x000000fa}, /* HISSC_STEREO_DLINK_L_PGA_INTEGER_CONFIG */
 {0,0x28,0x00000100,0x00000100}, /* HISSC_STEREO_DLINK_L_PGA_FRAC_CONFIG */
 {0,0x28,0x00000200,0x00000000}, /* HISSC_STEREO_DLINK_L_PGA_MUTE */
 {0,0x28,0x00003000,0x00000000}, /* HISSC_STEREO_DLINK_L_PGA_THR_CONFIG */
 {0,0x28,0x00ff0000,0x00fa0000}, /* HISSC_STEREO_DLINK_R_PGA_INTEGER_CONFIG */
 {0,0x28,0x01000000,0x01000000}, /* HISSC_STEREO_DLINK_R_PGA_FRAC_CONFIG */
 {0,0x28,0x02000000,0x00000000}, /* HISSC_STEREO_DLINK_R_PGA_MUTE */
 {0,0x28,0x30000000,0x00000000}, /* HISSC_STEREO_DLINK_R_PGA_THR_CONFIG */
 {0,0x58,0x0000ff00,0x00000000}, /* HISSC_DACR_AGC_USR_GDB_INTEGER */
 {0,0x50,0x0000ff00,0x00000000}, /* HISSC_DACL_AGC_USR_GDB_INTEGER */
 {0,0x8,0x00080000,0x00080000}, /* HISSC_DACL_HBF1I_EN */
 {0,0x8,0x00100000,0x00100000}, /* HISSC_DACL_HBF2I_EN */
 {0,0x8,0x08000000,0x08000000}, /* HISSC_DACR_HBF1I_EN */
 {0,0x8,0x10000000,0x10000000}, /* HISSC_DACR_HBF2I_EN */
 {0,0x4,0x00000010,0x00000010}, /* STEREO_DLINK_L_EN PGA */
 {0,0x8,0x00010000,0x00010000}, /* DACL_AGC_EN PGA */
 {0,0x5c,0x00000004,0x00000004}, /* SIF_DACL_EN PGA */
 {0,0x14,0x00020000,0x00000000}, /* unmute APB input */
 {0,0x4,0x00000020,0x00000020}, /* STEREO_DLINK_R_EN PGA */
 {0,0x8,0x01000000,0x01000000}, /* DACR_AGC_EN PGA */
 {0,0x5c,0x00000008,0x00000008}, /* SIF_DACR_EN PGA */
 {0,0x14,0x02000000,0x00000000}, /* unmute APB input */
 {0,0x60,0x00000010,0x00000000}, /* APB */
 {0,0x14,0x3f3f0000,0x0d0d0000}, /* AGASSI2 media-speaker mixer */
};
static u32 field_state(void __iomem **banks, int i)
{
	const struct digital_step *s=&digital[i];
	if (s->bank==1 && s->offset==4)
		return ~readl(banks[1]+8);
	if (s->bank==1 && s->offset==12)
		return readl(banks[1]+0x14);
	return readl(banks[s->bank]+s->offset);
}
static u32 access_path(void __iomem **banks, int i, u32 value)
{
	const struct digital_step *s=&digital[i];
	u32 old=field_state(banks,i);
	u32 set=(value&~old)&s->mask, clear=(old&~value)&s->mask;
	if (s->bank==1 && (s->offset==4 || s->offset==12)) {
		if (set)writel(set,banks[1]+s->offset);
		if (clear)writel(clear,banks[1]+(s->offset==4 ? 0 : 0x10));
	} else if (set||clear) {
		writel((old&~s->mask)|(value&s->mask),banks[s->bank]+s->offset);
	}
	field_state(banks,i);
	return old;
}
static const struct digital_step extra_gates[] = {
	{ 0, 0x08, 0x62620000, 0x62620000 }, /* L/R mixer, COMPI and SDM */
};

struct gate_state {
	u32 saved[ARRAY_SIZE(extra_gates)];
	unsigned int applied;
};

static int enable_extra_gates(void __iomem **banks, struct gate_state *state)
{
	for (int i = 0; i < ARRAY_SIZE(extra_gates); i++) {
		const struct digital_step *gate = &extra_gates[i];
		void __iomem *reg = banks[gate->bank] + gate->offset;
		u32 orig = readl(reg);

		state->saved[i] = orig;
		state->applied = i + 1;
		writel(orig | gate->mask, reg);
		if ((readl(reg) & gate->mask) != gate->mask)
			return -EIO;
	}
	return 0;
}

static int restore_extra_gates(void __iomem **banks, struct gate_state *state)
{
	int ret = 0;

	while (state->applied) {
		int i = --state->applied;
		const struct digital_step *gate = &extra_gates[i];
		void __iomem *reg = banks[gate->bank] + gate->offset;
		u32 orig = state->saved[i] & gate->mask;

		writel((readl(reg) & ~gate->mask) | orig, reg);
		if ((readl(reg) & gate->mask) != orig)
			ret = -EIO;
	}
	return ret;
}

/* Same 480 stereo writes as v095, into shared PCM RAM only. */
static bool guards(void __iomem *p, unsigned int end)
{
	return readl(p) == 0x55aa55aa && readl(p + 4) == 0x5a5a5a5a &&
	       readl(p + end - 8) == 0x55aa55aa && readl(p + end - 4) == 0x5a5a5a5a;
}

/* Same always-on 32 kHz timebase as the DSP; u32 differences handle wrap. */
static int t5_validate_slice(struct device *dev, struct t5_rx_irq *ctx)
{
	u64 start = ktime_get_ns();
	u32 ticks = readl(ctx->slice);
	u64 elapsed, expected;

	usleep_range(4000, 6000);
	ticks = readl(ctx->slice) - ticks;
	elapsed = ktime_get_ns() - start;
	expected = div_u64(elapsed * 32768, NSEC_PER_SEC);
	if (!ticks || ticks < expected * 3 / 4 || ticks > expected * 5 / 4 + 2)
		return dev_err_probe(dev, -ERANGE, "shared 32kHz counter: %u ticks in %llu us\n",
				     ticks, div_u64(elapsed, NSEC_PER_USEC));
	dev_info(dev, "shared 32kHz timer verified: %u ticks in %llu us; IRQ wait=%u\n",
		 ticks, div_u64(elapsed, NSEC_PER_USEC), ctx->wait_on_irq);
	return 0;
}

static int t5_submit_locked(struct device *dev, struct mbox_chan *chan, void __iomem *h,
		  void __iomem *q, const u32 *payload, unsigned int count, u32 seq,
		  struct timing_sample *sample, u32 mailcode)
{
	u32 front = readl(h + 8), header[9] = {0xa5a5a5a5, 0, 0, seq, 0, mailcode,
					       0, 0, count * 4};
	static u32 bell[8]; /* Vendor ACPU source 0 and HiFi message level 0. */
	int i, ret;

	struct t5_rx_irq *ctx = t5_rx(dev);

	if (front >= DATA_WORDS || readl(h + 12) != front)
		return -EBUSY;
	header[1] = readl(ctx->slice); /* per-message ulWriteSlice */
	for (i = 0; i < ARRAY_SIZE(header) + count; i++) {
		u32 v = i < ARRAY_SIZE(header) ? header[i] : payload[i - ARRAY_SIZE(header)];
		writel(v, q + 8 + 4 * front);
		front = (front + 1) % DATA_WORDS;
	}
	writel(readl(ctx->slice), h + 16); /* AP owns TX frontslice */
	wmb();
	if (sample)
		sample->publish_ns = ktime_get_ns();
	writel(front, h + 8);
	ret = mbox_send_message(chan, bell);
	if (ret < 0)
		return ret;
	for (i = 0; i < 250; i++) {
		if (readl(h + 12) == front) {
			if (sample)
				sample->consumed_ns = ktime_get_ns();
			mbox_client_txdone(chan, 0);
			if (payload[0] != 0xdd2c) dev_info(dev, "DSP consumed opcode=%04x seq=%u rear=%u\n", payload[0], seq, front);
			return 0;
		}
		usleep_range(3000, 5000);
	}
	dev_warn(dev, "opcode=%04x timeout front=%u rear=%u\n", payload[0], front, readl(h + 12));
	return -ETIMEDOUT;
}

static int submit_timed(struct device *dev, struct mbox_chan *chan, void __iomem *h,
        void __iomem *q, const u32 *payload, unsigned int count, u32 ignored_seq,
        struct timing_sample *sample, u32 mailcode)
{
    struct t5_board *board = dev_get_drvdata(dev);
    int ret;
    mutex_lock(&board->transmit);
    ret = t5_submit_locked(dev, chan, h, q, payload, count,
                           board->sequence++, sample, mailcode);
    mutex_unlock(&board->transmit);
    return ret;
}
/* AGS2-L03 audio.primary.hisi.so, soc_dsp_set_device +0xc8..+0xdc.
 * The HAL sends three words to /dev/hifi_misc. hifi_dsp_async_cmd appends
 * HIFI_CHN_SYNC_CMD and the reserved asynchronous sequence number.
 */
#define T5_HIFI_MISC_CODE  0x00040004
#define T5_DSP_SET_DEVICE  0xdd96

/* Selector 2: captured successful 12-byte ioctl from the board's audio HAL
 * (trace2.log). Selector 0: that same binary's used_modem_set_parameters
 * explicitly calls dsp_set_device(0, 0) at 0x31970..0x31978 to clear the
 * old selection before selecting its next route. Use that neutral command
 * for diagnostic teardown. The captured media standby sent no selector.
 * Keep the unused halfword zero as observed in the speaker command.
 * Only the explicitly enabled speaker test may call this helper.
 */
static const u32 t5_route_body[2][3] = {
	{ T5_DSP_SET_DEVICE, 0, 0 },
	{ T5_DSP_SET_DEVICE, 2, 0 },
};

static int t5_stock_route(struct device *dev, struct mbox_chan *chan,
			  void __iomem *tx, void __iomem *q, bool on,
			  u32 *tx_seq)
{
	u32 frame[5];

	if (t5_route_body[0][0] != T5_DSP_SET_DEVICE ||
	    t5_route_body[1][0] != T5_DSP_SET_DEVICE ||
	    t5_route_body[0][1] != 0 || t5_route_body[1][1] != 2 ||
	    t5_route_body[0][2] || t5_route_body[1][2]) {
		dev_warn(dev, "speaker route skipped: stock ON/OFF pair unverified\n");
		return -ENODATA;
	}
	memcpy(frame, t5_route_body[on], sizeof(t5_route_body[on]));
	frame[3] = 0;
	frame[4] = 0xffffffff;
	return submit_timed(dev, chan, tx, q, frame, ARRAY_SIZE(frame),
			    (*tx_seq)++, NULL, T5_HIFI_MISC_CODE);
}

static int submit(struct device *dev, struct mbox_chan *chan, void __iomem *h,
		  void __iomem *q, const u32 *payload, unsigned int count, u32 seq)
{
	return submit_timed(dev, chan, h, q, payload, count, seq, NULL, AUDIO_CODE);
}

static void t5_release_rx(struct t5_rx_irq *ctx, void __iomem *h, void __iomem *q,
			  u32 first, u32 words, u32 sent)
{
	u32 now = readl(ctx->slice);

	ctx->max_rx_age_ticks = max(ctx->max_rx_age_ticks, now - sent);
	/* The AP owns ulReadSlice in its current RX message and the RX rear
	 * slice in the shared head. Publish updates before releasing the slot. */
	writel(now, q + 8 + 4 * ((first + 2) % DATA_WORDS));
	writel(now, h + 20);
	wmb();
	writel((first + words) % DATA_WORDS, h + 12);
}

/* Two workers share one RX mailbox. The lock covers reading and releasing an
 * entire firmware message; each worker consumes only its own direction's queue.
 * IRQs wake both workers, including one waiting for an event moved by its peer. */
static int t5_dispatch_one(struct t5_rx_irq *ctx, void __iomem *h, void __iomem *q)
{
	u32 first = readl(h + 12), last = readl(h + 8), p[14], words;
	struct t5_events *queue;
	struct t5_event e = {};

	if (first >= DATA_WORDS || last >= DATA_WORDS)
		return -EBADMSG;
	if (first == last)
		return 0;
	if ((last + DATA_WORDS - first) % DATA_WORDS < 13)
		return -EBADMSG;
	for (int i = 0; i < 13; i++)
		p[i] = readl(q + 8 + 4 * ((first + i) % DATA_WORDS));
	if (p[0] != 0xa5a5a5a5 || p[5] != 0x04000003 ||
	    (p[8] != 16 && p[8] != 20) || p[10] > 1 ||
	    p[11] != 0x840001 + p[10] || p[12])
		return -EBADMSG;
	words = 9 + p[8]/4;
	if (words > (last + DATA_WORDS - first) % DATA_WORDS ||
	    (p[9] != 0xdda0 && (p[9] != 0xdd2d || p[8] != 20)))
		return -EBADMSG;
	e.mode = p[10];
	e.op = p[9];
	e.header_ticks = p[1];
	e.ticks = words == 14 ? readl(q + 8 + 4 * ((first + 13) % DATA_WORDS)) : 0;
	e.rx_ns = ktime_get_ns();
	queue = &ctx->msgs[e.mode];
	if (queue->head - queue->tail >= ARRAY_SIZE(queue->data))
		return -EOVERFLOW;
	queue->data[queue->head % ARRAY_SIZE(queue->data)] = e;
	WRITE_ONCE(queue->head, queue->head + 1);
	t5_release_rx(ctx, h, q, first, words, p[1]);
	return 1;
}

static int t5_next_event(struct device *dev, void __iomem *h, void __iomem *q,
			 unsigned int mode, struct t5_event *out, unsigned long deadline)
{
	struct t5_rx_irq *ctx = t5_rx(dev);
	struct t5_events *queue = &ctx->msgs[mode];
	int ret;
	for (;;) {
		long left;

		mutex_lock(&ctx->dispatch);
		ret = ctx->broken;
		while (!ret && queue->head == queue->tail) {
			int consumed = t5_dispatch_one(ctx, h, q);
			if (consumed <= 0) {
				ret = consumed;
				if (ret) WRITE_ONCE(ctx->broken, ret);
				break;
			}
			wake_up_all(&ctx->pending);
		}
		if (ret || queue->head != queue->tail) {
			if (!ret) {
				*out = queue->data[queue->tail % ARRAY_SIZE(queue->data)];
				WRITE_ONCE(queue->tail, queue->tail + 1);
			}
			mutex_unlock(&ctx->dispatch);
			if (ret) wake_up_all(&ctx->pending);
			return ret;
		}
		mutex_unlock(&ctx->dispatch);
		left = (long)(deadline-jiffies);
		if (left <= 0) return -ETIMEDOUT;
		wait_event_timeout(ctx->pending,
			READ_ONCE(queue->head) != READ_ONCE(queue->tail) ||
			READ_ONCE(ctx->broken) || readl(h+8) != readl(h+12), left);
	}
}

static int t5_wait_period(struct device *dev, void __iomem *h, void __iomem *q,
			  unsigned int mode, struct timing_sample *sample)
{
	struct t5_event e;
	int ret = t5_next_event(dev,h,q,mode,&e,jiffies+msecs_to_jiffies(1000));
	if (ret) return ret;
	if (e.op != 0xdd2d) return -EBADMSG;
	sample->rx_ns = e.rx_ns;
	sample->header_ticks = e.header_ticks;
	sample->dsp_ticks = e.ticks;
	return 0;
}

static int t5_wait_stop(struct device *dev, void __iomem *h, void __iomem *q,
			unsigned int mode)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(400);
	struct t5_event e;
	int ret;
	do {
		ret = t5_next_event(dev,h,q,mode,&e,deadline);
		if (ret) return ret;
		if (e.op == 0xdda0) return 0;
	} while (time_before(jiffies,deadline));
	return -ETIMEDOUT;
}
static u64 delta_us(u64 later, u64 earlier)
{
	return later >= earlier && earlier ? (later - earlier) / NSEC_PER_USEC : 0;
}

struct speaker_guard {
	struct gpio_desc **pa;
	struct timer_list cutoff;
	spinlock_t lock;
	bool expired;
};

static void speaker_quiet(struct gpio_desc **pa)
{
	gpiod_set_value(pa[0], 0);
	gpiod_set_value(pa[1], 0);
}

/* Both gpiod providers were checked for atomic-context operation at probe.
 * The timer can mute while the calling thread is blocked on the DSP. */
static void speaker_cutoff(struct timer_list *timer)
{
	struct speaker_guard *guard = container_of(timer, struct speaker_guard, cutoff);
	unsigned long flags;

	spin_lock_irqsave(&guard->lock, flags);
	WRITE_ONCE(guard->expired, true);
	speaker_quiet(guard->pa);
	spin_unlock_irqrestore(&guard->lock, flags);
}

static int speaker_start(struct speaker_guard *guard)
{
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&guard->lock, flags);
	mod_timer(&guard->cutoff, jiffies + msecs_to_jiffies(150));
	for (int i = 0; i < 2; i++) {
		gpiod_set_value(guard->pa[i], 1);
		udelay(2);
		gpiod_set_value(guard->pa[i], 0);
		udelay(2);
		gpiod_set_value(guard->pa[i], 1);
		udelay(2);
		if (gpiod_get_value(guard->pa[i]) != 1) {
			ret = -EIO;
			speaker_quiet(guard->pa);
			break;
		}
	}
	spin_unlock_irqrestore(&guard->lock, flags);
	return ret;
}

static void speaker_end(struct speaker_guard *guard)
{
	speaker_quiet(guard->pa);
	timer_delete_sync(&guard->cutoff);
}

static bool nominal_dsp_delta(u32 now, u32 before, unsigned int chunks)
{
	u32 ticks = now - before;

	return ticks >= 196 * chunks && ticks <= 492 * chunks; /* 6..15 ms at 32768 Hz */
}

static bool speaker_nominal(const struct timing_sample *a, const struct timing_sample *b, unsigned int chunks)
{
	u64 host = delta_us(b->rx_ns, a->rx_ns);
	bool dsp = nominal_dsp_delta(b->dsp_ticks, a->dsp_ticks, chunks);

	/* Some firmware omits msg_timestamp; the originating mailbox time is
	 * eligible only if both payload timestamps are zero. */
	if (!a->dsp_ticks && !b->dsp_ticks)
		dsp = nominal_dsp_delta(b->header_ticks, a->header_ticks, chunks);
	return host >= 6000 * chunks && host <= 15000 * chunks && dsp;
}

extern int hi6250_t5_asp_vote(bool on);
static bool asp_ready(struct device *dev, void __iomem *sctrl)
{
	u32 sub=readl(sctrl+0x16c), bus=readl(sctrl+0x18c), reset=readl(sctrl+0x208);
	dev_info(dev,"ASP gate=%08x bridge=%08x reset=%08x\n",sub,bus,reset);
	u32 power=readl(sctrl+0x68), isolation=readl(sctrl+0x48);
 dev_info(dev,"audio power=%08x isolation=%08x\n",power,isolation);
 return (power&BIT(0)) && !(isolation&BIT(0)) && (sub&BIT(26)) && (bus&0xf)==0xf && !(reset&0x780);
}

enum mic_bank { MIC_CODEC, MIC_ASP, MIC_PINS, MIC_PMU };
struct mic_field {
	u8 bank;
	u16 offset;
	u32 mask, value;
	u16 settle_ms;
};
#define MIC_PMU_FIELD(reg, mask, value, settle) \
	{ MIC_PMU, (reg) * 4, (mask), (value), (settle) }
static const struct mic_field mic_path[] = {
	/* ODM global settings for the ADC, also visible in stock mixer dump. */
	MIC_PMU_FIELD(0x261, 0x0c, 0x0c, 0),      /* both ADC clock edges */
	MIC_PMU_FIELD(0x262, 0x06, 0, 0),         /* ADC/SIF loopback off */
	MIC_PMU_FIELD(0x274, 0x7f, 0x12, 0),      /* ADCL FLSTN=1, bias=2 */
	MIC_PMU_FIELD(0x275, 0xff, 0x12, 0),      /* ADCR FLSTN=1, bias=2 */
	MIC_PMU_FIELD(0x283, 0xcc, 0x08, 0),      /* ADC chop clock=2 */
	MIC_PMU_FIELD(0x284, 0x0a, 0x00, 0),      /* mic/ADC chop enabled */

	/* hi6555c_power_codec() and hi6555c_smt_micb1_power_mode_event(). */
	MIC_PMU_FIELD(0x26b, 0x80, 0x00, 1),      /* MICB1_PD=0 */

	/* Select MIC (enum 1; 0 would mute), unmute, gain code 4 + boost. */
	MIC_PMU_FIELD(0x271, 0x7f, 0x38, 0),
	MIC_PMU_FIELD(0x272, 0x3f, 0x02, 0),      /* only MAINPGA -> ADCR */
	MIC_PMU_FIELD(0x273, 0x3f, 0x02, 0),      /* only MAINPGA -> ADCL */
	MIC_PMU_FIELD(0x26a, 0x08, 0x00, 0),      /* main PGA power up */
	MIC_PMU_FIELD(0x26a, 0x30, 0x00, 0),      /* both ADCs power up */
	MIC_PMU_FIELD(0x260, 0x0c, 0x0c, 0),      /* ADCs and SIF parent */

	/* SoC audio-capture: SIF -> physical CIC ADC, no synthesized source.
	 * ADC gain codes zero, both channels unmuted. FIFO threshold is a
	 * configuration register, not a FIFO data port. */
	{ MIC_CODEC, 0x0c, 0x00700000, 0x00400000 }, /* ADC sample rate 48 kHz */
	{ MIC_CODEC, 0x14, 0x00000077, 0 },       /* CIC_ADC both channels */
	{ MIC_CODEC, 0x1c, 0x33ff33ff, 0 },
	{ MIC_CODEC, 0x34, 0x00030000, 0x00030000 }, /* FIFO from ADCs */
	{ MIC_CODEC, 0x8c, 0x00000003, 0 },       /* A_U_L and A_U_R */
	{ MIC_CODEC, 0x3c, 0x1f1f1f1f, 0x01100110 }, /* full=16, empty=1 */
	{ MIC_CODEC, 0x5c, 0x00100073, 0x03 },    /* both ADCs, SIF, DMIC off */
	{ MIC_CODEC, 0x08, 0x00003f3f, 0x00003f3f }, /* L/R PGA & filters */
	{ MIC_CODEC, 0x04, 0x00000003, 0x00000003 }, /* stereo uplink L/R */
};
static const struct mic_field common_path[] = {
	/* The command registers have separate status registers. These two
	 * entries must succeed before accessing ANY SoC codec register. */
	{ MIC_ASP, 0x04, 0x600, 0x600 },
	{ MIC_ASP, 0x0c, 0x3000, 0x3000 },
	{ MIC_CODEC, 0x8c, BIT(4), BIT(4) },       /* stock third FIFO mux */
	{ MIC_CODEC, 0x00, 0x3c3c0000, 0x3c3c0000 },
	{ MIC_ASP, 0x1e0, 0x03, 0x03 },
	{ MIC_PINS, 0x2c, 7, 1 },
	{ MIC_PINS, 0x30, 7, 1 },
	{ MIC_PINS, 0x34, 7, 1 },
	{ MIC_PINS, 0x38, 7, 1 },
	{ MIC_CODEC, 0x88, 3, 2 },                 /* external 12.288 MHz */

	MIC_PMU_FIELD(0x26d, 0x03, 0x03, 5),      /* quick VREF */
	MIC_PMU_FIELD(0x26b, 0x01, 0x00, 0),      /* IBIAS_PD=0 */
	MIC_PMU_FIELD(0x26d, 0x02, 0x00, 0),      /* normal VREF */
    { MIC_CODEC, 0x5c, BIT(7), BIT(7) },
    MIC_PMU_FIELD(0x260, BIT(4), BIT(4), 0),
};
#undef MIC_PMU_FIELD

struct mic_route {
	u32 saved[ARRAY_SIZE(mic_path)];
	unsigned int applied;
};

static u32 mic_field_state(void __iomem **banks, const struct mic_field *f)
{
	if (f->bank == MIC_ASP && f->offset == 0x04)
		return ~readl(banks[MIC_ASP] + 0x08); /* bit set means in reset */
	if (f->bank == MIC_ASP && f->offset == 0x0c)
		return readl(banks[MIC_ASP] + 0x14);
	return readl(banks[f->bank] + f->offset);
}

static int mic_field_set(void __iomem **banks, const struct mic_field *f, u32 value)
{
	u32 old = mic_field_state(banks, f);
	u32 change = (old ^ value) & f->mask;
	u32 set = value & change, clear = old & change;

	if (!change)
		return 0;
	if (f->bank == MIC_ASP && (f->offset == 0x04 || f->offset == 0x0c)) {
		if (set)
			writel(set, banks[MIC_ASP] + f->offset);
		if (clear)
			writel(clear, banks[MIC_ASP] + (f->offset == 0x04 ? 0 : 0x10));
	} else {
		writel((old & ~f->mask) | (value & f->mask), banks[f->bank] + f->offset);
	}
	return ((mic_field_state(banks, f) ^ value) & f->mask) ? -EIO : 0;
}

static int mic_route_start(struct device *dev, void __iomem **banks, struct mic_route *route)
{
	if (route->applied)
		return -EBUSY;
	for (unsigned int i = 0; i < ARRAY_SIZE(mic_path); i++) {
		const struct mic_field *f = &mic_path[i];

		route->saved[i] = mic_field_state(banks, f) & f->mask;
		route->applied = i + 1;
		if (mic_field_set(banks, f, f->value)) {
			dev_err(dev, "capture route mismatch bank=%u offset=%x mask=%08x want=%08x\n",
				f->bank, f->offset, f->mask, f->value);
			return -EIO;
		}
		if (f->settle_ms)
			msleep(f->settle_ms);
	}
	/* Power-up on the stock driver pulses the two uplink reset bits then
	 * drains a CPU FIFO port. Reset only; DSP exclusively owns sample I/O.
	 * Retain other low control bits and never echo high error/status bits. */
	u32 ctrl = readl(banks[MIC_CODEC] + 0x30) & 0x1ff;
	if (ctrl & 3)
		return -EBUSY;
	writel(ctrl | 3, banks[MIC_CODEC] + 0x30);
	readl(banks[MIC_CODEC] + 0x30);
	udelay(1);
	writel(ctrl, banks[MIC_CODEC] + 0x30);
	if (readl(banks[MIC_CODEC] + 0x30) & 3)
		return -EIO;

	dev_info(dev, "media-main-mic verified: MICB1, MAINPGA to ADCL+ADCR, 48kHz stereo uplink\n");
	return 0;
}

static int mic_route_stop(struct device *dev, void __iomem **banks, struct mic_route *route)
{
	int ret = 0;
	while (route->applied) {
		unsigned int i = --route->applied;
		const struct mic_field *f = &mic_path[i];
		if (mic_field_set(banks, f, route->saved[i])) {
			dev_err(dev, "capture restore mismatch bank=%u offset=%x mask=%08x\n",
				f->bank, f->offset, f->mask);
			ret = -EIO;
		}
	}
	return ret;
}


#define CAPTURE_BASE 0x35035c00U
#define CAPTURE_OFFSET 0x3c00U
#define CAPTURE_BYTES (960U * 2 * sizeof(s16))
#define CAPTURE_SLOTS 12U
#define CAPTURE_RING (CAPTURE_BYTES * CAPTURE_SLOTS)
#define MISC_CODE T5_HIFI_MISC_CODE
static int set_mic_source(struct device *dev, struct mbox_chan *chan,
			  void __iomem *tx, void __iomem *q, u32 *seq, bool on)
{
	u32 body[] = {0xdd95, on ? 1 : 0, 0, 0xffffffff};
	return submit_timed(dev, chan, tx, q, body, ARRAY_SIZE(body), (*seq)++, NULL, MISC_CODE);
}

enum t5_phase { T5_PREPARED, T5_RUNNING, T5_STOPPED };

struct t5_stream {
	struct snd_pcm_substream *sub;
	atomic_t phase;
	wait_queue_head_t command;
	struct completion ready, done;
	struct mutex lifecycle;
	bool owned;
	int setup_result;
	snd_pcm_uframes_t cursor; /* consumed frames modulo ALSA boundary */
	snd_pcm_uframes_t position; /* same cursor modulo the ALSA ring */
};

/* A committed region cannot be overwritten by ordinary ALSA writers before
 * period_elapsed() releases it. Ignore bytes after appl_ptr, including the
 * final uncommitted tail of a short stream. */
static snd_pcm_uframes_t t5_available(struct t5_stream *s)
{
	struct snd_pcm_runtime *rt = s->sub->runtime;
	snd_pcm_uframes_t app = READ_ONCE(rt->control->appl_ptr);
	if (app < s->cursor)
		app += rt->boundary;
	return app - s->cursor;
}

static int t5_copy_period(struct t5_stream *s, void __iomem *dst)
{
	struct snd_pcm_runtime *rt = s->sub->runtime;
	snd_pcm_uframes_t avail = t5_available(s);
	unsigned int count, i;
	const __le32 *ring = (const __le32 *)rt->dma_area;

	if (avail < T5_FRAMES &&
	    snd_pcm_get_state(s->sub) != SNDRV_PCM_STATE_DRAINING) {
		/* A client that starts with only one period can be woken by
		 * our preceding period_elapsed(). Allow one short refill window. */
		wait_event_timeout(s->command,
			t5_available(s) >= T5_FRAMES ||
			snd_pcm_get_state(s->sub) == SNDRV_PCM_STATE_DRAINING ||
			atomic_read(&s->phase) != T5_RUNNING,
			max_t(long, msecs_to_jiffies(4), 1));
		avail = t5_available(s);
	}
	if (atomic_read(&s->phase) != T5_RUNNING)
		return 0;
	if (!avail || avail > rt->buffer_size ||
	    (avail < T5_FRAMES && snd_pcm_get_state(s->sub) != SNDRV_PCM_STATE_DRAINING))
		return -EPIPE;
	count = min_t(snd_pcm_uframes_t, avail, T5_FRAMES);
	smp_rmb();
	for (i = 0; i < count; i++)
		writel(le32_to_cpu(ring[(s->position+i) % rt->buffer_size]), dst + i*4);
	for (; i < T5_FRAMES; i++)
		writel(0, dst + i*4);
	wmb();
	return count;
}

static int t5_stream_periods(struct device *dev, struct t5_stream *s,
			     struct mbox_chan *chan, void __iomem *tx,
			     void __iomem *rx, void __iomem *q, void __iomem *pcm,
			     struct gpio_desc **pa, u32 *seq)
{
	struct snd_pcm_runtime *rt = s->sub->runtime;
	struct speaker_guard guard = { .pa = pa };
	struct timing_sample previous = {};
	unsigned int streak = 0, slot = 0, pending = 0;
	u32 next[] = {0xdd2c, 0, 0, T5_PERIOD_BYTES};
	bool ready = false, active = false;
	int ret = 0;

	spin_lock_init(&guard.lock);
	timer_setup_on_stack(&guard.cutoff, speaker_cutoff, 0);
	for (unsigned int warm = 0; atomic_read(&s->phase) != T5_STOPPED;) {
		struct timing_sample current_period = {};
		ret = t5_wait_period(dev, rx, q + RING_BYTES, 0, &current_period);
		if (ret || atomic_read(&s->phase) == T5_STOPPED)
			break;
		if (ready && atomic_read(&s->phase) == T5_RUNNING &&
		    !speaker_nominal(&previous, &current_period, 2)) {
			ret = -ETIME;
			break;
		}
		if (!ready) {
			streak = previous.rx_ns && speaker_nominal(&previous, &current_period, 2)
				? streak + 1 : 0;
			if (streak >= 8) {
				ready = true;
				s->setup_result = 0;
				complete(&s->ready);
			} else if (++warm == 32) {
				ret = -ERANGE;
				break;
			}
		}
		previous = current_period;
		if (pending) {
			s->cursor = (s->cursor + pending) % rt->boundary;
			WRITE_ONCE(s->position, (s->position + pending) % rt->buffer_size);
			snd_pcm_period_elapsed(s->sub);
			pending = 0;
			if (atomic_read(&s->phase) != T5_RUNNING)
				break;
		}
		slot ^= 1;
		next[2] = slot * T5_PERIOD_BYTES;
		/* Keep the two slots silent while prepared. The first eight
		 * firmware periods qualify the physical route before any audio. */
		if (ready && atomic_read(&s->phase) == T5_RUNNING) {
			ret = t5_copy_period(s, pcm + next[2]);
			if (ret <= 0)
				break;
			pending = ret;
			if (!active) {
				ret = speaker_start(&guard);
				if (ret) break;
				active = true;
			}
			if (READ_ONCE(guard.expired)) {
				ret = -ETIMEDOUT;
				break;
			}
			mod_timer(&guard.cutoff, jiffies + msecs_to_jiffies(150));
		}
		ret = submit(dev, chan, tx, q, next, ARRAY_SIZE(next), (*seq)++);
		if (ret)
			break;
	}
	speaker_end(&guard);
	timer_destroy_on_stack(&guard.cutoff);
	return ret;
}
/* The resource mutex protects first/last-user transitions and all codec
 * register read/modify/write operations. It is separate from the TX and RX
 * locks, so ramp delays cannot block a running peer's period updates. */
struct t5_bus {
	void *group;
	struct mbox_chan *chan;
	void __iomem *tx, *rx, *q, *sctrl, *banks[4], *nv_area;
	void __iomem *dma[2];
	u8 *nv_before;
	u32 common_saved[ARRAY_SIZE(common_path)];
	unsigned int common_applied, users;
	bool nv_written, asp_voted;
};

struct t5_direction {
	unsigned int mode, slots, bytes;
	struct mic_route mic;
	struct gate_state gates;
	u8 analog_saved[ARRAY_SIZE(path)];
	int analog_count, digital_count;
	u32 digital_saved[ARRAY_SIZE(digital)];
	struct gpio_desc *pa[2];
	u8 *dma_before;
	bool region_saved, opened, selected, started, stopped;
};

/* Stock bootloader HiFi image status on AGS2-L03. */
#define T5_HIFI_IMAGE_STATUS 0x351c5014

static void t5_init_ring(void __iomem *head, void __iomem *data)
{
	memset_io(data, 0, RING_BYTES);
	writel(0x55aa55aa, data);
	writel(0x5a5a5a5a, data + 4);
	writel(0x55aa55aa, data + RING_BYTES - 8);
	writel(0x5a5a5a5a, data + RING_BYTES - 4);
	writel(0, head + 8);
	writel(0, head + 12);
	writel(0, head + 16);
	writel(0, head + 20);
	writel(0x55aa55aa, head + 32);
	writel(0x5a5a5a5a, head + 36);
	writel(0x5a5a5a5a, head + 4);
	wmb();
	writel(0x55aa55aa, head);
}

static bool t5_ring_idle(void __iomem *head)
{
	return !readl(head + 8) && !readl(head + 12);
}

static bool t5_ring_ready(void __iomem *head, void __iomem *data)
{
	return guards(head, 40) && guards(data, RING_BYTES);
}

static int t5_prepare_mailboxes(struct device *dev, void __iomem *tx,
			       void __iomem *rx, void __iomem *q)
{
	struct t5_board *board = dev_get_drvdata(dev);
	void __iomem *rx_data = q + RING_BYTES;
	void __iomem *status;
	u32 image_status;

	if (t5_ring_ready(tx, q) && t5_ring_ready(rx, rx_data))
		return 0;

	/* The stock AP initializes the outgoing ring on its first use. */
	if (!guards(tx, 40) && t5_ring_idle(tx) && t5_ring_idle(rx) &&
	    t5_ring_ready(rx, rx_data)) {
		t5_init_ring(tx, q);
		return t5_ring_ready(tx, q) && t5_ring_idle(tx) ? 0 : -EIO;
	}

	/* On a cold boot the vendor receiver normally initializes RX first.
	 * Only initialize both if every guard set is absent and the DSP image
	 * status indicates success. Preserve a partially initialized mailbox.
	 */
	if (board->mailbox_init_attempted || guards(tx, 40) || guards(rx, 40) ||
	    guards(q, RING_BYTES) || guards(rx_data, RING_BYTES))
		return -EINVAL;

	if ((readl(tx) & readl(rx) & readl(q) & readl(rx_data)) == U32_MAX)
		return -EIO;

	status = ioremap(T5_HIFI_IMAGE_STATUS, sizeof(u32));
	if (!status)
		return -ENOMEM;
	image_status = readl(status);
	iounmap(status);
	if (image_status)
		return dev_err_probe(dev, -ENODEV, "HiFi image status=%08x\n",
				     image_status);

	board->mailbox_init_attempted = true;
	t5_init_ring(rx, rx_data);
	if (!t5_ring_ready(rx, rx_data) || !t5_ring_idle(rx))
		return dev_err_probe(dev, -EIO, "HiFi cold RX readback failed\n");

	t5_init_ring(tx, q);
	if (!t5_ring_ready(tx, q) || !t5_ring_idle(tx))
		return dev_err_probe(dev, -EIO, "HiFi cold TX readback failed\n");

	dev_info(dev, "HiFi cold RX and TX initialized and verified\n");
	return 0;
}

static void t5_release_channel(void *chan) { mbox_free_channel(chan); }

static int t5_bus_init(struct device *dev, struct t5_bus *bus)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct mbox_client *client;
	struct mbox_chan *chan;
	struct clk *soc, *modem;
	struct t5_rx_irq *rxirq;
	void __iomem *base, *tx, *rx, *q, *banks[4], *pmu, *sctrl, *nv_area=NULL;
	const struct firmware *nv_fw=NULL;
	u8 *nv_before=NULL, *readback=NULL;
	bool nv_written=false;
	u32 first, last;
	int ret=t5_peri_acquire(dev);

	if (ret) return ret;
		modem=devm_clk_get_enabled(dev,"modem");
	if (IS_ERR(modem)) return dev_err_probe(dev,PTR_ERR(modem),"modem clock\n");
	soc=devm_clk_get_enabled(dev,"codec");
	if (IS_ERR(soc)) return dev_err_probe(dev,PTR_ERR(soc),"codec clock\n");
	dev_info(dev,"clocks %lu / %lu Hz\n",clk_get_rate(modem),clk_get_rate(soc));
	pmu=devm_ioremap(dev,0xfff34000,0x1000);
	if (!pmu || !clk_get_rate(soc)) return -ENODEV;
	banks[0]=devm_ioremap(dev,0xe8052000,0x1000);
	banks[1]=devm_ioremap(dev,0xe804e000,0x400);
	banks[2]=devm_ioremap(dev,0xfff11000,0x1000);
	banks[3]=pmu;
	if (!banks[0] || !banks[1] || !banks[2]) return -ENOMEM;
	{
		int irq=platform_get_irq(pdev,0);
		if (irq<0)return irq;
		rxirq=devm_kzalloc(dev,sizeof(*rxirq),GFP_KERNEL);
		if (!rxirq)return -ENOMEM;
		rxirq->irq = irq;
		rxirq->wait_on_irq = true;
		mutex_init(&rxirq->dispatch);
		init_waitqueue_head(&rxirq->pending);
		sctrl = devm_ioremap(dev, 0xfff0a000, SZ_4K);
		if (!sctrl)
			return -ENOMEM;
		rxirq->slice = sctrl + 0x534;
		ret = t5_validate_slice(dev, rxirq);
		if (ret)
			return ret;
		((struct t5_board *)dev_get_drvdata(dev))->rx = rxirq;
		rxirq->chan=devm_ioremap(dev,0xe896b000+2*64,64);
		if (!rxirq->chan)return -ENOMEM;
		dev_info(dev,"RX2 initial mode=%08x src=%08x mask=%08x\n",
			readl(rxirq->chan+0x10),readl(rxirq->chan),readl(rxirq->chan+0x14));
		ret=devm_request_irq(dev,irq,hifi_doorbell,0,"t5-hifi-period",rxirq);
		if (ret)return dev_err_probe(dev,ret,"RX2 IRQ\n");
	}
	client = devm_kzalloc(dev, sizeof(*client), GFP_KERNEL);
	if (!client)
		return -ENOMEM;
	client->dev = dev;
	client->knows_txdone = true;
	chan = mbox_request_channel(client, 0);
	if (IS_ERR(chan))
		return PTR_ERR(chan);
	ret = devm_add_action_or_reset(dev, t5_release_channel, chan);
    if (ret) return ret;
    base = devm_ioremap(dev, HIFI_MB_BASE, SZ_64K);
	if (!base) {
		ret = -ENOMEM;
		return ret;
	}
	ret = -EINVAL;

	tx = base + 80;
	rx = base + 120;
	q = base + 1200;
	dev_info(dev, "tx front=%u rear=%u rx front=%u rear=%u\n",
		readl(tx+8), readl(tx+12), readl(rx+8), readl(rx+12));
	dev_info(dev, "header guards TX=%d RX=%d; reference data guards TX=%d RX=%d\n",
		guards(tx,40), guards(rx,40), guards(q,RING_BYTES), guards(q+RING_BYTES,RING_BYTES));
	ret = t5_prepare_mailboxes(dev, tx, rx, q);
	if (ret)
		return ret;
	ret = -EINVAL;
	first = readl(tx + 12);
	last = readl(tx + 8);
	if (first >= DATA_WORDS || last >= DATA_WORDS || readl(rx + 8) != readl(rx + 12))
		return ret;
	if (first != last) {
		u32 packet[11];
		int i;
		if ((last + DATA_WORDS - first) % DATA_WORDS != ARRAY_SIZE(packet))
			return ret;
		for (i = 0; i < ARRAY_SIZE(packet); i++)
			packet[i] = readl(q + 8 + 4 * ((first + i) % DATA_WORDS));
		dev_info(dev, "inherited frame code=%08x len=%u opcode=%08x\n",
			 packet[5], packet[8], packet[9]);
		if (packet[0] != 0xa5a5a5a5 || packet[5] != AUDIO_CODE ||
		    packet[8] != 8 || packet[9] != 0xdd26 || packet[10] > 1)
			return ret;
	}
	/* Same boot-time head initialization as the stock driver, restricted
	 * to an empty receive ring and an empty or verified stale CLOSE. */
	writel(0, tx + 8);
	writel(0, tx + 12);
	writel(0, tx + 16);
	writel(0, tx + 20);
    bus->chan=chan; bus->tx=tx; bus->rx=rx; bus->q=q; bus->sctrl=sctrl;
    memcpy(bus->banks,banks,sizeof(banks));
	{
		ret=request_firmware_direct(&nv_fw,"t5/ags2l03-hifi-nv.bin",dev);
		if (ret)goto nv_done;
		if (nv_fw->size!=NV_LEN || memcmp(nv_fw->data,"\xaa\x55\xaa\x55",4) ||
		    memcmp(nv_fw->data+NV_LEN-4,nv_fw->data,4)) { ret=-EBADMSG;goto nv_done; }
		nv_before=vmalloc(NV_LEN);
		readback=vmalloc(NV_LEN);
		nv_area=devm_ioremap(dev,NV_ADDR,NV_LEN);
		if (!nv_area || !nv_before || !readback) {ret=-ENOMEM;goto nv_done;}
		memcpy_fromio(nv_before,nv_area,NV_LEN);
		write_nv(nv_area,nv_fw->data);
		nv_written=true;
		memcpy_fromio(readback,nv_area,NV_LEN);
		if (memcmp(readback,nv_fw->data,NV_LEN)) {ret=-EIO;goto nv_done;}
		dev_info(dev,"board NV readback verified bytes=%d before_crc=%08x active_crc=%08x\n",
		         NV_LEN,crc32_le(0,nv_before,NV_LEN),crc32_le(0,readback,NV_LEN));
	}
nv_done:
    bus->nv_area = nv_area;
    bus->nv_before = nv_before;
    bus->nv_written = nv_written;
    release_firmware(nv_fw);
    vfree(readback);
    return ret;

}

static int t5_bus_finish(struct device *dev, struct t5_bus *bus)
{
	int ret = 0;

	while (bus->common_applied) {
		unsigned int i=--bus->common_applied;
		if (mic_field_set(bus->banks,&common_path[i],bus->common_saved[i]))
			ret=-EIO;
	}
	if (bus->asp_voted && hi6250_t5_asp_vote(false))
		ret=-EIO;
	if (bus->nv_written) {
		write_nv(bus->nv_area,bus->nv_before);
		/* Exact restoration without needing another large allocation. */
		for (unsigned int i=0; i<NV_LEN; i+=4)
			if (readl(bus->nv_area+i) != get_unaligned_le32(bus->nv_before+i)) {
				ret=-EIO;
				break;
			}
	}
	vfree(bus->nv_before);
	devres_release_group(dev,bus->group);
	((struct t5_board *)dev_get_drvdata(dev))->rx=NULL;
	kfree(bus);
	return ret;
}

static int t5_bus_get(struct device *dev, struct t5_bus **out)
{
	struct t5_board *b=dev_get_drvdata(dev);
	struct t5_bus *bus=b->bus;
	int ret;

	if (!bus) {
		bus=kzalloc(sizeof(*bus),GFP_KERNEL);
		if (!bus) return -ENOMEM;
		bus->group=devres_open_group(dev,NULL,GFP_KERNEL);
		if (!bus->group) { kfree(bus); return -ENOMEM; }
		ret=t5_bus_init(dev,bus);
		if (!ret) {
			bus->dma[0]=devm_ioremap(dev,0x35032000,T5_BUF_BYTES);
			bus->dma[1]=devm_ioremap(dev,CAPTURE_BASE,CAPTURE_RING);
			if (!bus->dma[0] || !bus->dma[1]) ret=-ENOMEM;
		}
		devres_close_group(dev,bus->group);
		if (ret) { t5_bus_finish(dev,bus); return ret; }
		b->bus=bus;
		b->sequence=0;
	}
	bus->users++;
	*out=bus;
	return 0;
}

static int t5_common_enable(struct device *dev, struct t5_bus *bus)
{
	int ret;
	if (bus->common_applied == ARRAY_SIZE(common_path)) return 0;
	if (bus->asp_voted) return -EIO;
	ret=hi6250_t5_asp_vote(true);
	if (ret) return ret;
	bus->asp_voted=true;
	msleep(20);
	if (!asp_ready(dev,bus->sctrl)) return -EAGAIN;
	for (unsigned int i=0;i<ARRAY_SIZE(common_path);i++) {
		const struct mic_field *f=&common_path[i];
		bus->common_saved[i]=mic_field_state(bus->banks,f) & f->mask;
		bus->common_applied=i+1;
		ret=mic_field_set(bus->banks,f,f->value);
		if (ret) return ret;
		if (f->settle_ms) msleep(f->settle_ms);
	}
	return 0;
}

static int t5_direction_route(struct device *dev,struct t5_bus *bus,
			      struct t5_direction *d)
{
	int ret;
	if (d->mode)
		return mic_route_start(dev,bus->banks,&d->mic);
	for (unsigned int i=0; i<2; i++) {
		struct gpio_desc *pa=gpiod_get_index(dev,"speaker",i,GPIOD_OUT_LOW);
		if (IS_ERR(pa)) return PTR_ERR(pa);
		d->pa[i]=pa;
		if (gpiod_cansleep(pa)) return -EINVAL;
	}
	for (unsigned int i=0; i<ARRAY_SIZE(digital); i++) {
		d->digital_saved[i]=access_path(bus->banks,i,digital[i].value);
		d->digital_count++;
		if ((field_state(bus->banks,i)&digital[i].mask)!=digital[i].value)
			return -EIO;
	}
	ret=configure_analog(dev,bus->banks[3],d->analog_saved,&d->analog_count);
	if (!ret) ret=enable_extra_gates(bus->banks,&d->gates);
	return ret;
}

static int t5_restore_route(struct device *dev,struct t5_bus *bus,
			    struct t5_direction *d)
{
	int ret=0;
	if (d->mode) return mic_route_stop(dev,bus->banks,&d->mic);
	for (unsigned int i=0;i<2;i++)
		if (d->pa[i]) {
			gpiod_set_value(d->pa[i],0);
			if (gpiod_get_value(d->pa[i])) ret=-EIO;
			gpiod_put(d->pa[i]);
		}
	if (restore_extra_gates(bus->banks,&d->gates)) ret=-EIO;
	if (restore_analog(bus->banks[3],d->analog_saved,d->analog_count)) ret=-EIO;
	while (d->digital_count) {
		unsigned int i=--d->digital_count;
		access_path(bus->banks,i,d->digital_saved[i]);
		if (((field_state(bus->banks,i)^d->digital_saved[i])&digital[i].mask))
			ret=-EIO;
	}
	return ret;
}

static int t5_mic_loop(struct device *dev,struct t5_bus *bus,struct t5_stream *s)
{
	struct snd_pcm_runtime *rt=s->sub->runtime;
	unsigned int slot=0;
	u32 next[]={0xdd2c,1,0,T5_PERIOD_BYTES}, seq=0, last=0;
	bool have_stamp=false;

	while (atomic_read(&s->phase)==T5_RUNNING) {
		struct timing_sample sample={};
		snd_pcm_uframes_t app, unread;
		int ret=t5_wait_period(dev,bus->rx,bus->q+RING_BYTES,1,&sample);
		if (ret || atomic_read(&s->phase)!=T5_RUNNING) return ret;
		if (have_stamp && !nominal_dsp_delta(sample.dsp_ticks,last,2))
			return -ETIME;
		have_stamp=true;
		last=sample.dsp_ticks;
		app=READ_ONCE(rt->control->appl_ptr);
		unread=s->cursor >= app ? s->cursor-app : rt->boundary-app+s->cursor;
		if (unread > rt->buffer_size-T5_FRAMES) return -EPIPE;
		memcpy_fromio(rt->dma_area+s->position*4,bus->dma[1]+slot*T5_PERIOD_BYTES,
			      T5_PERIOD_BYTES);
		smp_wmb();
		s->cursor=(s->cursor+T5_FRAMES)%rt->boundary;
		WRITE_ONCE(s->position,(s->position+T5_FRAMES)%rt->buffer_size);
		snd_pcm_period_elapsed(s->sub);
		if (atomic_read(&s->phase)!=T5_RUNNING) break;
		slot=(slot+1)%CAPTURE_SLOTS;
		next[2]=CAPTURE_OFFSET+slot*T5_PERIOD_BYTES;
		ret=submit(dev,bus->chan,bus->tx,bus->q,next,ARRAY_SIZE(next),seq++);
		if (ret) return ret;
	}
	return 0;
}

static int t5_select(struct device *dev,struct t5_bus *bus,
		     struct t5_direction *d,bool on,u32 *seq)
{
	if (d->mode) return set_mic_source(dev,bus->chan,bus->tx,bus->q,seq,on);
	return t5_stock_route(dev,bus->chan,bus->tx,bus->q,on,seq);
}

static int t5_run(struct t5_stream *s)
{
	struct device *dev=s->sub->pcm->card->dev;
	struct t5_board *b=dev_get_drvdata(dev);
	struct t5_bus *bus;
	struct t5_direction *d;
	unsigned int mode=s->sub->stream, slots=mode ? CAPTURE_SLOTS : 2;
	u32 open[]={0xdd25,mode,2,48000,T5_FRAMES,slots,0}, close[]={0xdd26,mode};
	u32 params[]={0xdd27,mode,2,48000,0}, seq=0;
	u32 start[]={0xdd2a,mode,1,0x840001+mode,0,mode ? CAPTURE_OFFSET : 0,T5_PERIOD_BYTES};
	u32 stop[]={0xdd2a,mode,0,0x840001+mode,0,0,0};
	int ret,cleanup;

	d=kzalloc(sizeof(*d),GFP_KERNEL);
	if (!d) return -ENOMEM;
	d->mode=mode; d->slots=slots; d->bytes=slots*T5_PERIOD_BYTES;
	d->dma_before=kvmalloc(d->bytes,GFP_KERNEL);
	if (!d->dma_before) { kfree(d); return -ENOMEM; }
	mutex_lock(&b->resources);
	if (b->unavailable || atomic_read(&s->phase)==T5_STOPPED) {
		ret=-ENODEV;
		goto unlock;
	}
	ret=t5_bus_get(dev,&bus);
	if (ret) goto unlock;
	memcpy_fromio(d->dma_before,bus->dma[mode],d->bytes);
	d->region_saved=true;
	memset_io(bus->dma[mode],0,d->bytes);
	wmb();
	d->opened=true; /* Also send CLOSE on a lost OPEN reply. */
	ret=submit(dev,bus->chan,bus->tx,bus->q,open,ARRAY_SIZE(open),seq++);
	if (ret) goto finish;
	ret=t5_common_enable(dev,bus);
	if (ret) goto finish;
	if (atomic_read(&s->phase)==T5_STOPPED) goto finish;
	ret=t5_direction_route(dev,bus,d);
	if (ret) goto finish;
	d->selected=true;
	ret=t5_select(dev,bus,d,true,&seq);
	if (ret) goto finish;
	ret=submit(dev,bus->chan,bus->tx,bus->q,params,ARRAY_SIZE(params),seq++);
	if (ret || atomic_read(&s->phase)==T5_STOPPED) goto finish;
	d->started=true;
	ret=submit(dev,bus->chan,bus->tx,bus->q,start,ARRAY_SIZE(start),seq++);
	mutex_unlock(&b->resources);
	if (!ret) {
		if (mode) ret=t5_mic_loop(dev,bus,s);
		else ret=t5_stream_periods(dev,s,bus->chan,bus->tx,bus->rx,bus->q,
					   bus->dma[0],d->pa,&seq);
	}
	mutex_lock(&b->resources);
finish:
	if (d->started) {
		cleanup=submit(dev,bus->chan,bus->tx,bus->q,stop,ARRAY_SIZE(stop),seq++);
		if (!cleanup) cleanup=t5_wait_stop(dev,bus->rx,bus->q+RING_BYTES,mode);
		d->stopped=!cleanup;
		if (!ret) ret=cleanup;
	}
	if (d->selected) {
		cleanup=t5_select(dev,bus,d,false,&seq);
		if (!ret) ret=cleanup;
	}
	cleanup=t5_restore_route(dev,bus,d);
	if (!ret) ret=cleanup;
	if (d->opened) {
		cleanup=submit(dev,bus->chan,bus->tx,bus->q,close,ARRAY_SIZE(close),seq++);
		if (!ret) ret=cleanup;
	}
	if (d->region_saved && (!d->started || d->stopped)) {
		memcpy_toio(bus->dma[mode],d->dma_before,d->bytes);
		wmb();
		for (unsigned int i=0; i<d->bytes; i+=4) {
			if (readl(bus->dma[mode]+i)!=get_unaligned_le32(d->dma_before+i)) {
				ret=-EIO;
				break;
			}
		}
	}
	if (!--bus->users) {
		b->bus=NULL;
		cleanup=t5_bus_finish(dev,bus);
		if (!ret) ret=cleanup;
	}
	dev_info(dev,"%s session closed result=%d peer_users=%u\n",
		 mode ? "capture" : "playback",ret,b->bus ? b->bus->users : 0);
unlock:
	mutex_unlock(&b->resources);
	kvfree(d->dma_before);
	kfree(d);
	return ret;
}
static int t5_worker(void *arg)
{
	struct t5_stream *s = arg;
	struct device *dev = s->sub->pcm->card->dev;
    int ret = 0;
    if (s->sub->stream == SNDRV_PCM_STREAM_CAPTURE) {
        /* Enumerating/opening or preparing a source never enables MICB1. */
        s->setup_result = 0;
        complete(&s->ready);
        wait_event(s->command, atomic_read(&s->phase) != T5_PREPARED);
    }
    if (atomic_read(&s->phase) != T5_STOPPED) {
        ret = t5_run(s);
    }
	if (s->setup_result == -EINPROGRESS) {
		s->setup_result = ret ? ret : -EIO;
		complete(&s->ready);
	} else if (ret) {
		/* If the engine faults between prepare and START, prevent a
		 * successful trigger on the already-exited thread. */
		if (atomic_cmpxchg(&s->phase, T5_PREPARED, T5_STOPPED) == T5_RUNNING) {
			dev_warn_ratelimited(dev, "PCM %d stopped: %d\n", s->sub->stream, ret);
			snd_pcm_stop_xrun(s->sub);
		}
	}
	complete(&s->done);
	return 0;
}

static const struct snd_pcm_hardware t5_hw = {
	.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BATCH,
	.formats = SNDRV_PCM_FMTBIT_S16_LE, .rates = SNDRV_PCM_RATE_48000,
	.rate_min = 48000, .rate_max = 48000, .channels_min = 2, .channels_max = 2,
	.buffer_bytes_max = T5_BUF_BYTES,
	.period_bytes_min = T5_PERIOD_BYTES, .period_bytes_max = T5_PERIOD_BYTES,
	.periods_min = 2, .periods_max = 2,
};

static int t5_pcm_open(struct snd_pcm_substream *sub)
{
	struct t5_stream *s = &((struct t5_stream *)sub->pcm->private_data)[sub->stream];
	struct t5_board *board = sub->pcm->card->private_data;
	if (READ_ONCE(board->unavailable))
		return -ENODEV;
	sub->runtime->hw = t5_hw;
    if (sub->stream == SNDRV_PCM_STREAM_CAPTURE) {
        sub->runtime->hw.periods_min = sub->runtime->hw.periods_max = CAPTURE_SLOTS;
        sub->runtime->hw.buffer_bytes_max = CAPTURE_RING;
    }
	sub->runtime->private_data = s;
	s->sub = sub;
	return 0;
}

/* Called in process context, outside the PCM stream spinlock. */
static void t5_join_locked(struct t5_stream *s)
{
	if (!s->owned)
		return;
	atomic_set(&s->phase, T5_STOPPED);
	wake_up(&s->command);
	wait_for_completion(&s->done);
	s->owned = false;
}

static int t5_join(struct snd_pcm_substream *sub)
{
	struct t5_stream *s = sub->runtime->private_data;
	mutex_lock(&s->lifecycle);
	t5_join_locked(s);
	mutex_unlock(&s->lifecycle);
	return 0;
}


static int t5_hw_params(struct snd_pcm_substream *sub, struct snd_pcm_hw_params *params)
{
    /* Managed PCM buffers are allocated before this callback. Never expose
     * a previous client's samples when the ALSA core reuses vmalloc pages. */
    if (sub->stream == SNDRV_PCM_STREAM_CAPTURE) {
        if (!sub->runtime->dma_area || params_buffer_bytes(params) != CAPTURE_RING)
            return -EINVAL;
        memset(sub->runtime->dma_area, 0, CAPTURE_RING);
    }
    return 0;
}

static int t5_release_pcm(struct snd_pcm_substream *sub)
{
    int ret = t5_join(sub);
    if (sub->stream == SNDRV_PCM_STREAM_CAPTURE && sub->runtime->dma_area)
        memset(sub->runtime->dma_area, 0, sub->runtime->dma_bytes);
    return ret;
}

static int t5_pcm_prepare(struct snd_pcm_substream *sub)
{
	struct t5_stream *s = sub->runtime->private_data;
	struct t5_board *board = sub->pcm->card->private_data;
	struct snd_pcm_runtime *rt = sub->runtime;
	struct task_struct *task;
	int ret;

	if (rt->period_size != T5_FRAMES || rt->buffer_size != (sub->stream == SNDRV_PCM_STREAM_CAPTURE ? CAPTURE_SLOTS : 2) * T5_FRAMES ||
	    rt->rate != 48000 || rt->channels != 2 ||
	    rt->format != SNDRV_PCM_FORMAT_S16_LE || !rt->dma_area)
		return -EINVAL;
	mutex_lock(&s->lifecycle);
	t5_join_locked(s);
	if (READ_ONCE(board->unavailable)) {
		mutex_unlock(&s->lifecycle);
		return -ENODEV;
	}
	atomic_set(&s->phase, T5_PREPARED);
	reinit_completion(&s->ready);
	reinit_completion(&s->done);
	s->cursor = 0;
	WRITE_ONCE(s->position, 0);
	s->setup_result = -EINPROGRESS;
	task = kthread_run(t5_worker, s, "t5-hifi-pcm");
	if (IS_ERR(task)) {
		ret = PTR_ERR(task);
	} else {
		s->owned = true;
		wait_for_completion(&s->ready);
		ret = s->setup_result;
		if (ret)
			t5_join_locked(s);
	}
	mutex_unlock(&s->lifecycle);
	return ret;
}

static int t5_pcm_trigger(struct snd_pcm_substream *sub, int cmd)
{
	struct t5_stream *s = sub->runtime->private_data;
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		if (atomic_cmpxchg(&s->phase, T5_PREPARED, T5_RUNNING) != T5_PREPARED)
			return -EPIPE;
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		atomic_set(&s->phase, T5_STOPPED);
		break;
	default:
		return -EINVAL;
	}
	wake_up(&s->command);
	return 0;
}

static int t5_pcm_ack(struct snd_pcm_substream *sub)
{
	struct t5_stream *s = sub->runtime->private_data;
	wake_up(&s->command);
	return 0;
}

static snd_pcm_uframes_t t5_pcm_pointer(struct snd_pcm_substream *sub)
{
	struct t5_stream *s = sub->runtime->private_data;
	return READ_ONCE(s->position);
}

static const struct snd_pcm_ops t5_ops = {
	.open = t5_pcm_open, .close = t5_release_pcm, .hw_params = t5_hw_params, .ioctl = snd_pcm_lib_ioctl,
	.prepare = t5_pcm_prepare, .trigger = t5_pcm_trigger,
	.sync_stop = t5_join, .hw_free = t5_release_pcm,
	.pointer = t5_pcm_pointer, .ack = t5_pcm_ack,
};

/* The card may outlive the platform remove callback while user files close. */
struct t5_card_state {
    struct t5_board board;
    struct t5_stream streams[2];
};
static int t5_pcm_probe(struct platform_device *pdev)
{
	struct snd_card *card;
	struct snd_pcm *pcm;
	struct t5_board *board;
	struct t5_stream *s;
	int ret;

	ret = snd_devm_card_new(&pdev->dev, -1, "T5HiFi", THIS_MODULE,
				sizeof(struct t5_card_state), &card);
	if (ret) return ret;
	board = card->private_data;
	board->card = card;
	platform_set_drvdata(pdev, board);
	mutex_init(&board->resources);
    mutex_init(&board->transmit);
    s = ((struct t5_card_state *)card->private_data)->streams;
	board->stream = s;
	for (int i=0; i<2; i++) {
	init_completion(&s[i].ready);
	init_completion(&s[i].done);
	mutex_init(&s[i].lifecycle);
	init_waitqueue_head(&s[i].command);
	}
	strscpy(card->driver, "T5HiFi", sizeof(card->driver));
	strscpy(card->shortname, "MediaPad T5 Audio", sizeof(card->shortname));
	strscpy(card->longname, "Huawei MediaPad T5 HiFi DSP / Hi6555C Audio",
		sizeof(card->longname));
	ret = snd_pcm_new(card, "Internal Audio", 0, 1, 1, &pcm);
	if (ret) return snd_card_free_on_error(&pdev->dev, ret);
	board->pcm = pcm;
	pcm->private_data = s;
	strscpy(pcm->name, "Internal Audio", sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &t5_ops);
    snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &t5_ops);
    for (int dir=0; dir<2; dir++) {
        size_t size = dir ? CAPTURE_RING : T5_BUF_BYTES;
        ret = snd_pcm_set_managed_buffer(pcm->streams[dir].substream,
                                             SNDRV_DMA_TYPE_VMALLOC, NULL, size, size);
        if (ret) break;
    }
	if (!ret) ret = snd_card_register(card);
	return snd_card_free_on_error(&pdev->dev, ret);
}

static void t5_pcm_shutdown(struct platform_device *pdev)
{
	struct t5_board *b = platform_get_drvdata(pdev);
	if (!b || !b->pcm) return;
	WRITE_ONCE(b->unavailable, true);
	snd_pcm_suspend_all(b->pcm);
    for (int i=0; i<2; i++) {
        struct t5_stream *s=&b->stream[i];
        mutex_lock(&s->lifecycle);
        t5_join_locked(s);
        mutex_unlock(&s->lifecycle);
    }
}

static void t5_pcm_remove(struct platform_device *pdev)
{
	struct t5_board *b = platform_get_drvdata(pdev);
	WRITE_ONCE(b->unavailable, true);
	snd_card_disconnect(b->card);
    for (int i=0; i<2; i++) {
        struct t5_stream *s=&b->stream[i];
        mutex_lock(&s->lifecycle);
        t5_join_locked(s);
        mutex_unlock(&s->lifecycle);
    }
}

static int t5_system_suspend(struct device *dev)
{
	struct t5_board *b = dev_get_drvdata(dev);
	snd_power_change_state(b->card, SNDRV_CTL_POWER_D3hot);
	t5_pcm_shutdown(to_platform_device(dev));
	return 0;
}

static int t5_system_resume(struct device *dev)
{
	struct t5_board *b = dev_get_drvdata(dev);
	WRITE_ONCE(b->unavailable, false);
	snd_power_change_state(b->card, SNDRV_CTL_POWER_D0);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(t5_pm, t5_system_suspend, t5_system_resume);

static const struct of_device_id t5_ids[] = {
	{ .compatible = "huawei,t5-hifi-ram-test" }, {}
};
static struct platform_driver t5_hifi_driver = {
	.probe = t5_pcm_probe, .remove = t5_pcm_remove, .shutdown = t5_pcm_shutdown,
	.driver = { .name = "t5-hifi-pcm", .of_match_table = t5_ids,
		    .pm = pm_sleep_ptr(&t5_pm) },
};
builtin_platform_driver(t5_hifi_driver);
