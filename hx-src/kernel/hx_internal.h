/* SPDX-License-Identifier: GPL-2.0 */
/* HX 内核内部接口 */
#ifndef _HX_INTERNAL_H
#define _HX_INTERNAL_H

#include <linux/types.h>
#include <linux/sched.h>
#include "hx.h"

extern struct hx_cfg g_cfg;

struct hx_stat {
	u64 ticks;
	u64 last_us;
	u64 read_bytes;
	u64 injected;
	s32 last_pid;
};
extern struct hx_stat g_stat;

/* 符号解析（kprobe 拿 kallsyms_lookup_name） */
int  hx_sym_init(void);
void *hx_sym(const char *name);

/* 进程 / 模块 */
int  hx_pid_by_name(const char *name);
int  hx_module_range(int pid, const char *so, u64 *base, u64 *end);
int  hx_module_range_any(int pid, const char *so, u64 *base, u64 *end, bool *rw);

/* 内存 */
int  hx_read(int pid, u64 addr, void *buf, u32 len);
int  hx_write(int pid, u64 addr, const void *buf, u32 len);
int  hx_read_u64(int pid, u64 addr, u64 *out);
int  hx_read_u32(int pid, u64 addr, u32 *out);
int  hx_read_fx(int pid, u64 addr, fx_t *out);
fx_t hx_f32_to_fx(u32 bits);

/* 采集 */
int  hx_scan_once(struct hx_ent *out, int cap);
int  hx_self_pos(fx_t *x, fx_t *z);

/* 绘制 */
int  hx_drm_init(void);
void hx_drm_exit(void);
void hx_drm_frame(const struct hx_ent *e, int n, int self_team);
bool hx_drm_ready(void);

/* 触摸注入 */
int  hx_touch_init(void);
void hx_touch_exit(void);
int  hx_touch_swipe(int x1, int y1, int x2, int y2, int ms);
int  hx_touch_tap(int x, int y, int ms);

/* 配置 */
void hx_cfg_default(struct hx_cfg *c);
void hx_cfg_apply(const struct hx_cfg *c);

/* 功能总开关 */
void hx_feature_resume(void);
void hx_feature_suspend(void);

/* 控制通道（__NR_reboot kprobe 后门） */
int  hx_gate_init(void);
void hx_gate_exit(void);

/* 隐身 */
int  hx_hide_self(void);

/* 日志（HX_FLAG_NO_LOG 下完全静默） */
void hx_warn(const char *fmt, ...);

#endif
