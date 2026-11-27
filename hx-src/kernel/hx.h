/* SPDX-License-Identifier: GPL-2.0 */
/*
 * HX (幻形) — 内核态常驻辅助核心 · 共享协议头
 *
 * 重要约束：内核态禁用浮点（-mgeneral-regs-only），任何 float 运算都会生成
 * __addsf3 / __mulsf3 / fmaf 之类的软浮点调用，而内核没有 libgcc，链接必挂。
 * 所以本文件里所有坐标/角度都用 Q16.16 定点整数表示。
 *
 * 定点约定：
 *   1.0 == FX_ONE == 65536
 *   世界坐标 x / z 来自游戏里的 IEEE754 float，由 hx_f32_to_fx() 用纯整数换算
 */
#ifndef _HX_H
#define _HX_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t  s32;
typedef int64_t  s64;
typedef float    f32;
#endif

/* ---------------- 定点数 ---------------- */
typedef s32 fx_t;
#define FX_SHIFT      16
#define FX_ONE        (1 << FX_SHIFT)          /* 65536 */
#define FX_INT(x)     ((fx_t)((x) << FX_SHIFT))
#define FX_TRUNC(x)   ((s32)((x) >> FX_SHIFT))
#define FX_MUL(a, b)  ((fx_t)(((s64)(a) * (b)) >> FX_SHIFT))
#define FX_DIV(a, b)  ((fx_t)((((s64)(a)) << FX_SHIFT) / (b)))

#define HX_VERSION      0x03020000u
#define HX_ABI          4

/* ------------------------------------------------------------------ *
 *  控制通道（__NR_reboot 后门）
 * ------------------------------------------------------------------ */
#define HX_GATE_M1      0x484C5831u   /* "HLX1" */
#define HX_GATE_M2      0x5A17C0DEu

struct hx_gate {
	u32 magic;
	u32 version;
	u32 cmd;
	u32 key;
	u64 arg0;
	u64 arg1;
	u64 arg2;
	u64 out0;
	u64 out1;
	u32 pad[4];
};

enum hx_cmd {
	HX_CMD_NOP      = 0,
	HX_CMD_PING     = 1,
	HX_CMD_PID      = 2,
	HX_CMD_MODULE   = 3,
	HX_CMD_READ     = 4,
	HX_CMD_WRITE    = 5,
	HX_CMD_STAT     = 6,
	HX_CMD_CFG      = 7,
	HX_CMD_UNBIND   = 8,
};

#define hx_rw_pack(ptr, len)  ((((u64)(len) & 0xFFFFull) << 48) | \
			       ((u64)(ptr) & 0x0000FFFFFFFFFFFFull))
#define hx_rw_ptr(p)          ((u64)(p) & 0x0000FFFFFFFFFFFFull)
#define hx_rw_len(p)          ((u32)(((u64)(p) >> 48) & 0xFFFFu))

/* ------------------------------------------------------------------ *
 *  运行时配置（hxctl cfg 下发）
 *  注意：坐标/角度都是整数或定点，没有 float
 * ------------------------------------------------------------------ */
struct hx_layer {
	u32 on;
	u32 color;
	s32 thick;
	s32 pad;
};

struct hx_cfg {
	u32 abi;
	u32 flags;
	char pkg[32];
	char core_so[24];
	char world_so[24];

	/* ---- 锚点与结构偏移 ---- */
	u64 an_off;
	u32 o_mgr_ent;
	u32 o_ent_head;
	u32 o_ent_next;
	u32 o_ent_actor;
	u32 o_actor_x;
	u32 o_actor_z;
	u32 o_actor_hp;
	u32 o_actor_team;
	u32 o_actor_camp;
	u32 ent_max;
	u32 hud_w;
	u32 hud_h;

	/* ---- 世界->屏幕（0 = 关闭；内核态无浮点，暂不实现矩阵投影）---- */
	u64 w2s_addr;
	u32 w2s_stride;
	u32 w2s_rows;

	/* ---- 雷达 ---- */
	s32 radar_cx;
	s32 radar_cy;
	s32 radar_r;
	s32 world_span;        /* 世界半宽（米），整数 */
	s32 radar_rot_deg;     /* 旋转（度），当前未使用 */

	/* ---- 自瞄 ---- */
	u32 aim_on;
	u32 aim_key;
	s32 aim_fov;           /* 触发距离（米） */
	s32 aim_smooth;
	s32 aim_lead_ms;
	s32 aim_slot_px;

	/* ---- 绘制层 ---- */
	struct hx_layer ly_box;
	struct hx_layer ly_line;
	struct hx_layer ly_hp;
	struct hx_layer ly_radar;
	struct hx_layer ly_text;

	u32 tick_ms;
	u32 max_read;
	u32 pad2[8];
};

enum {
	HX_FLAG_LOOSE_CLOCK = 1u << 0,
	HX_FLAG_HIDE_SELF   = 1u << 1,
	HX_FLAG_NO_LOG      = 1u << 2,
	HX_FLAG_CAMP_FILTER = 1u << 3,
	HX_FLAG_DRM_ALPHA   = 1u << 4,
	/*
	 * DRM 直绘总开关，默认关闭。
	 * kprobe pre_handler 在原子上下文，dma_buf_begin_cpu_access() 这种
	 * 会睡眠的调用绝不能放进去，否则开机早期挂上钩子立刻
	 * "scheduling while atomic" -> panic -> bootloop。
	 * 先确认内核能正常启动、采集正常，再用 hxctl 打开这一位。
	 */
	HX_FLAG_DRM_DRAW    = 1u << 5,
};

enum { HX_AIM_ALWAYS = 1, HX_AIM_TOUCH = 2, HX_AIM_KEY = 4 };

#define HX_READ_CHUNK  (4096ull)
#define HX_MAX_ENT     64

/* 实体快照：坐标是 Q16.16 定点 */
struct hx_ent {
	u64 actor;
	fx_t x, z;
	s32 hp, team;
};

#endif /* _HX_H */
