// SPDX-License-Identifier: GPL-2.0
/*
 * hx_touch.c — 内核态输入注入
 *
 * 为什么不用 uinput / input 命令 / TwT 的 ioctl 触摸：
 *   - uinput 会多出一个 /dev/uinput 打开的进程，ACE 扫 fd 就能看见；
 *   - `input tap` 走 shell，cmdline 一眼看穿；
 *   - TwT 的 _IOW('T',6/7/8) 在本机实测返回失败。
 *
 * HX 直接对真实触摸设备的 input_dev 调 input_event()，事件从内核原生
 * 路径进入 evdev，时间戳由内核自己打，和手指按下去产生的序列在
 * input 子系统的视角完全同源。
 *
 * 设备发现方式（针对这台设备实测过的符号可用性）：
 *   内核开了 CONFIG_TRIM_UNUSED_KSYMS，input_class 这个数据符号不在导出区里，
 *   所以不走 class_find_device(&input_class, ...)，改成用 kallsyms 拿
 *   input_dev_list 的地址直接遍历链表 —— 零导出符号依赖。
 *   （input_event 本身确认导出 ✅，直接调用即可）
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/device.h>
#include <linux/list.h>
#include <linux/ktime.h>
#include <linux/delay.h>
#include <linux/jiffies.h>

#include "hx.h"
#include "hx_internal.h"

static struct list_head *g_input_list;
static struct input_dev  *g_touch;
static int g_slot = 1;          /* slot 0 常被系统手势占用，用 slot 1 */
static int g_tracking = 0x4800;

static bool hx_is_touch(struct input_dev *idev)
{
	if (!idev)
		return false;
	if (!test_bit(EV_ABS, idev->evbit))
		return false;
	if (!test_bit(ABS_MT_POSITION_X, idev->absbit))
		return false;
	if (!test_bit(ABS_MT_POSITION_Y, idev->absbit))
		return false;
	/* 排除 stylus / hover 类 */
	if (idev->name && (strstr(idev->name, "Pen") || strstr(idev->name, "pen")))
		return false;
	return true;
}

static void hx_touch_pick(void)
{
	struct input_dev *idev;

	if (g_touch || !g_input_list)
		return;
	list_for_each_entry(idev, g_input_list, node) {
		if (hx_is_touch(idev)) {
			g_touch = idev;
			return;
		}
	}
}

int hx_touch_init(void)
{
	if (!g_input_list) {
		g_input_list = hx_sym("input_dev_list");
		if (!g_input_list) {
			/* 个别内核把 input_dev_list 编进 .rodata 后 kallsyms 没有，
			 * 退一步用 input_class（若它恰好导出）*/
			g_input_list = NULL;
		}
	}
	hx_touch_pick();
	return g_touch ? 0 : -ENODEV;
}

void hx_touch_exit(void)
{
	g_touch = NULL;
}

static void hx_emit(int type, int code, int val)
{
	if (!g_touch)
		return;
	input_event(g_touch, type, code, val);
}

static void hx_syn(void)
{
	hx_emit(EV_SYN, SYN_REPORT, 0);
}

/* 屏幕像素 -> 设备原始坐标（用 absinfo 归一化，不同机型通吃） */
static void hx_to_dev(int px, int py, int *dx, int *dy)
{
	int xmin = 0, xmax = g_cfg.hud_w ? g_cfg.hud_w : 1280;
	int ymin = 0, ymax = g_cfg.hud_h ? g_cfg.hud_h : 2760;

	/* struct input_dev 没有 absmin/absmax，范围在 absinfo[axis].minimum/maximum */
	if (g_touch && g_touch->absinfo) {
		const struct input_absinfo *ai = g_touch->absinfo;

		xmin = ai[ABS_MT_POSITION_X].minimum;
		xmax = ai[ABS_MT_POSITION_X].maximum;
		ymin = ai[ABS_MT_POSITION_Y].minimum;
		ymax = ai[ABS_MT_POSITION_Y].maximum;
	}
	if (xmax <= xmin)
		xmax = xmin + 1;
	if (ymax <= ymin)
		ymax = ymin + 1;

	*dx = xmin + (int)((s64)px * (xmax - xmin) /
			   (g_cfg.hud_w ? g_cfg.hud_w : 1280));
	*dy = ymin + (int)((s64)py * (ymax - ymin) /
			   (g_cfg.hud_h ? g_cfg.hud_h : 2760));
}

static void hx_touch_down(int x, int y)
{
	int dx, dy;

	hx_to_dev(x, y, &dx, &dy);
	g_tracking = (g_tracking + 7) & 0x7FFF;
	if (!g_tracking)
		g_tracking = 0x4800;

	hx_emit(EV_ABS, ABS_MT_SLOT, g_slot);
	hx_emit(EV_ABS, ABS_MT_TRACKING_ID, g_tracking);
	hx_emit(EV_ABS, ABS_MT_POSITION_X, dx);
	hx_emit(EV_ABS, ABS_MT_POSITION_Y, dy);
	/* 补压力/触摸面积，缺这两项部分机型会被上层判定为"幽灵触摸" */
	if (g_touch && test_bit(ABS_MT_PRESSURE, g_touch->absbit))
		hx_emit(EV_ABS, ABS_MT_PRESSURE, 64);
	if (g_touch && test_bit(ABS_MT_TOUCH_MAJOR, g_touch->absbit))
		hx_emit(EV_ABS, ABS_MT_TOUCH_MAJOR, 8);
	hx_syn();
}

static void hx_touch_move(int x, int y)
{
	int dx, dy;

	hx_to_dev(x, y, &dx, &dy);
	hx_emit(EV_ABS, ABS_MT_SLOT, g_slot);
	hx_emit(EV_ABS, ABS_MT_POSITION_X, dx);
	hx_emit(EV_ABS, ABS_MT_POSITION_Y, dy);
	hx_syn();
}

static void hx_touch_up(void)
{
	hx_emit(EV_ABS, ABS_MT_SLOT, g_slot);
	hx_emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
	hx_syn();
}

/*
 * 拖拽：按 ms 时长拆成 ~120Hz 的中间点。
 * 用 ease-out 采样，避免"匀速度机器人"特征。
 */
int hx_touch_swipe(int x1, int y1, int x2, int y2, int ms)
{
	int steps, i;
	ktime_t t0;

	if (!g_touch)
		return -ENODEV;
	if (ms < 16)
		ms = 16;
	steps = ms / 8;
	if (steps < 2)
		steps = 2;
	if (steps > 240)
		steps = 240;

	hx_touch_down(x1, y1);
	t0 = ktime_get();
	for (i = 1; i <= steps; i++) {
		float t = (float)i / (float)steps;
		float ease = 1.f - (1.f - t) * (1.f - t);
		int cx = x1 + (int)((x2 - x1) * ease);
		int cy = y1 + (int)((y2 - y1) * ease);
		u64 target = (u64)(ms * 1000) * i / steps;
		s64 now;

		hx_touch_move(cx, cy);
		now = ktime_us_delta(ktime_get(), t0);
		if ((s64)target > now)
			usleep_range(target - now, target - now + 200);
	}
	hx_touch_up();
	g_stat.injected++;
	return 0;
}

int hx_touch_tap(int x, int y, int ms)
{
	if (!g_touch)
		return -ENODEV;
	if (ms < 20)
		ms = 20;
	hx_touch_down(x, y);
	usleep_range(ms * 1000, ms * 1000 + 3000);
	hx_touch_up();
	g_stat.injected++;
	return 0;
}
