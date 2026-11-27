// SPDX-License-Identifier: GPL-2.0
/*
 * hx_main.c — HX 主模块
 *
 * 生命周期（顺序不能变，摘链之后模块就不在链表里了，但代码仍可执行）：
 *   sym_init -> cfg_default -> touch_init -> drm_init -> gate_init
 *   -> feature_resume -> hide_self
 *
 * 运行模型：
 *   一个 delayed_work 以 cfg.tick_ms（默认 16ms）节拍跑：
 *     采样实体 -> 推给 DRM 绘制 -> 跑自瞄决策（需要时注入触摸）
 *   全部在进程上下文，可以安全 usleep_range / dma_buf_vmap。
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/math.h>
#include <linux/string.h>
#include <linux/random.h>
#include <linux/delay.h>

/* 诊断：1 = 只跑 init，不启动采样线程 */
#define HX_DIAG_NO_TICK 1

#include "hx.h"
#include "hx_internal.h"

struct hx_cfg g_cfg;
struct hx_stat g_stat;

static struct delayed_work g_tick_work;
static bool g_running;
static bool g_drm_ok;      /* drm_atomic_commit 钩子是否已挂上 */

/* ------------------------------------------------------------------ */
void hx_warn(const char *fmt, ...)
{
	va_list ap;

	if (g_cfg.flags & HX_FLAG_NO_LOG)
		return;
	va_start(ap, fmt);
	vprintk(fmt, ap);
	va_end(ap);
}

void hx_cfg_default(struct hx_cfg *c)
{
	memset(c, 0, sizeof(*c));
	c->abi = HX_ABI;
	c->flags = HX_FLAG_HIDE_SELF | HX_FLAG_NO_LOG | HX_FLAG_LOOSE_CLOCK |
		   HX_FLAG_CAMP_FILTER | HX_FLAG_DRM_ALPHA;

	strncpy(c->pkg, "com.tencent.tmgp.sgame", sizeof(c->pkg) - 1);
	strncpy(c->core_so, "libGameCore.so", sizeof(c->core_so) - 1);
	strncpy(c->world_so, "libil2cpp.so", sizeof(c->world_so) - 1);

	c->an_off      = 0x182810;
	c->o_mgr_ent   = 0x00;
	c->o_ent_head  = 0x1D0;
	c->o_ent_next  = 0x150;
	c->o_ent_actor = 0xF8;
	c->o_actor_x   = 0x3C;
	c->o_actor_z   = 0x40;
	c->o_actor_hp  = 0;      /* 定位到后再下发 */
	c->o_actor_team = 0;
	c->o_actor_camp = 0;
	c->ent_max     = 16;

	c->hud_w       = 1280;
	c->hud_h       = 2760;

	c->w2s_addr    = 0;

	c->radar_cx    = 250;
	c->radar_cy    = 250;
	c->radar_r     = 160;
	c->world_span  = 200;      /* 米 */
	c->radar_rot_deg = 0;    /* 度，当前未使用 */

	c->aim_on      = 0;
	c->aim_key     = HX_AIM_TOUCH;
	c->aim_fov     = 260;
	c->aim_smooth  = 35;
	c->aim_lead_ms = 60;
	c->aim_slot_px = 3;

	c->ly_box.on = 1;   c->ly_box.color = 0xC0FF3B3B; c->ly_box.thick = 2;
	c->ly_line.on = 0;  c->ly_line.color = 0x60FFFFFF;
	c->ly_hp.on = 1;    c->ly_hp.color = 0xFF30FF30;
	c->ly_radar.on = 1; c->ly_radar.color = 0xA03BE0FF;
	c->ly_text.on = 0;  c->ly_text.color = 0xFFFFFFFF;

	c->tick_ms = 16;
	c->max_read = 0x10000;
}

void hx_cfg_apply(const struct hx_cfg *src)
{
	struct hx_cfg c = *src;   /* 先在栈上拷一份，避免下发过程中被 tick 读到 */

	if (c.tick_ms < 4)
		c.tick_ms = 4;
	if (c.tick_ms > 200)
		c.tick_ms = 200;
	if (c.ent_max == 0 || c.ent_max > HX_MAX_ENT)
		c.ent_max = HX_MAX_ENT;
	if (c.max_read < 64 || c.max_read > 0x100000)
		c.max_read = 0x10000;
	if (c.world_span < 10 || c.world_span > 2000)
		c.world_span = 200;
	if (c.radar_r < 8)
		c.radar_r = 160;
	if (c.hud_w < 320)
		c.hud_w = 1280;
	if (c.hud_h < 320)
		c.hud_h = 2760;

	memcpy(&g_cfg, &c, sizeof(c));
}

/* ------------------------------------------------------------------ *
 * 自瞄
 *   目标评分：距离优先，血量低加权（可配）。
 *   输出：把落点算成"摇杆方向"，或者直接用 w2s 坐标点击。
 *   抗检测：落点按 aim_slot_px 做随机抖动；拖拽时长按距离给不同值，
 *           不做"永远精准到 1px"的机械运动。
 * ------------------------------------------------------------------ */
#if 0  /* single-file test: 这些函数引用其他 .c 的符号 */
static int hx_pick_target(const struct hx_ent *e, int n)
{
	int i, best = -1;
	s64 best_score = (s64)1 << 62;

	for (i = 1; i < n; i++) {
		s64 dx, dz, d2, score;

		if ((g_cfg.flags & HX_FLAG_CAMP_FILTER) && e[i].team >= 0 &&
		    e[0].team >= 0 && e[i].team == e[0].team)
			continue;

		dx = e[i].x - e[0].x;
		dz = e[i].z - e[0].z;
		/* 距离平方（Q16.16 米 -> s64，防溢出），再降一档避免越界 */
		d2 = ((dx * dx + dz * dz) >> FX_SHIFT) >> 8;
		score = d2;
		/* 血量越低越优先：score * (4000+hp)/8000 */
		if (e[i].hp > 0)
			score = score * (4000 + e[i].hp) / 8000;
		if (score < best_score) {
			best_score = score;
			best = i;
		}
	}
	return best;
}

static void hx_aim_tick(const struct hx_ent *e, int n)
{
	int t = hx_pick_target(e, n);
	int jx, jy, ms;
	s32 dx, dz, dist_m;
	u32 d2;

	if (t < 0)
		return;
	/* hx_touch_init() 返回 0 表示成功 */
	if (hx_touch_init() != 0)
		return;

	dx = e[t].x - e[0].x;
	dz = e[t].z - e[0].z;
	/* 开方：Q16.16 米^2 -> 米。先 >>16 再 >>8 降到 u32，最后 >>4 还原单位 */
	{
		s64 d2fx = ((s64)dx * dx + (s64)dz * dz) >> FX_SHIFT;

		d2 = (u32)(d2fx >> 8);
		dist_m = (s32)(int_sqrt(d2) >> 4);
	}
	if (dist_m < 1)
		dist_m = 1;

	/* 目标超出 fov（米）就放弃 */
	if (dist_m > g_cfg.aim_fov)
		return;

	jx = (int)get_random_u32_below(g_cfg.aim_slot_px * 2 + 1) -
	     g_cfg.aim_slot_px;
	jy = (int)get_random_u32_below(g_cfg.aim_slot_px * 2 + 1) -
	     g_cfg.aim_slot_px;

	/* 摇杆中心：实测 PJZ110 是 (480,885)，按屏幕宽度等比缩放 */
	{
		int cx = g_cfg.hud_w * 480 / 1280;
		int cy = g_cfg.hud_h * 885 / 2760;
		int range = g_cfg.hud_w / 6;
		s64 den = (s64)dist_m * FX_ONE;
		int tx = cx + (int)((s64)dx * range / den);
		int ty = cy + (int)((s64)dz * range / den);

		ms = 60 + dist_m * 4;
		if (ms > 320)
			ms = 320;
		if (ms < 60)
			ms = 60;

		hx_touch_swipe(cx, cy, tx + jx, ty + jy, ms);
	}
}

/* ------------------------------------------------------------------ */

static void hx_tick_fn(struct work_struct *w)
{
	static const struct hx_ent dummy[HX_MAX_ENT];
	struct hx_ent e[HX_MAX_ENT];
	ktime_t t0;
	int n;

	if (!g_running)
		return;

	t0 = ktime_get();

	/*
	 * msm_drm 是 vendor 模块，由 init 在 late_initcall 之后加载。
	 * 编进内核时 hx 的 initcall 比它早，第一次 register_kprobe 会找不到
	 * drm_atomic_commit 符号。这里每 ~2 秒重试一次，模块一加载就能挂上。
	 */
	if (!g_drm_ok && (g_stat.ticks % 125) == 1)
		g_drm_ok = (hx_drm_init() == 0);

	n = hx_scan_once(e, HX_MAX_ENT);
	if (n > 0) {
		hx_drm_frame(e, n, 0);
		if (g_cfg.aim_on)
			hx_aim_tick(e, n);
	} else {
		hx_drm_frame(dummy, 0, 0);
	}

	g_stat.ticks++;
	g_stat.last_us = (u64)ktime_us_delta(ktime_get(), t0);

	if (g_running)
		queue_delayed_work(system_wq, &g_tick_work,
				   msecs_to_jiffies(g_cfg.tick_ms));
}

void hx_feature_resume(void)
{
	if (g_running)
		return;
	g_running = true;
	/*
	 * 首次延迟 15 秒：等 Android 的启动流程走完再开始采样。
	 * 开机早期各种子系统都在初始化，此时去遍历 mm / 注册 kprobe
	 * 容易和它们抢锁，把风险窗口挪到系统稳定之后。
	 */
	queue_delayed_work(system_wq, &g_tick_work, msecs_to_jiffies(15000));
}

void hx_feature_suspend(void)
{
	g_running = false;
	cancel_delayed_work_sync(&g_tick_work);
}

#endif  /* single-file test */

static int __init hx_init(void)
{
	/* 单文件测试：不碰任何其他 .c，只验证 hx 存在本身是否会挂 */
	pr_info("hx: single-file test\n");
	return 0;
}


#if 0  /* exit */
static void __exit hx_exit(void)
{
	hx_feature_suspend();
	hx_gate_exit();
	hx_drm_exit();
	hx_touch_exit();
}
#endif

#ifdef MODULE
module_init(hx_init);
module_exit(hx_exit);
#else
/*
 * 编进内核（obj-y）时用 late_initcall：
 * module_init 对应 device_initcall（第 6 级），比 vendor 模块加载还早，
 * 那时 msm_drm / input 子系统可能都没就绪。late_initcall 更晚一档，
 * 配合 hx_tick_fn 里的重试，能覆盖模块后加载的情况。
 */
late_initcall(hx_init);
#endif

MODULE_LICENSE("GPL");
MODULE_AUTHOR("hx");
MODULE_DESCRIPTION("display pipeline helper");
MODULE_VERSION("3.1.0");
