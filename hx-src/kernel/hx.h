/* SPDX-License-Identifier: GPL-2.0 */
/*
 * HX (幻形) — 内核态常驻辅助核心
 * 共享协议头：kernel <-> userspace (hxctl 一次性控制台)
 *
 * 设计要点（相对 sgame / Sevik 的差异）：
 *   1. 不创建设备节点、不用 ioctl、不留 fd —— 控制通道走 __NR_reboot 的
 *      kprobe 前置钩子（pre_handler 改 x0 直接返回），ACE 作为普通 app
 *      连 CAP_SYS_BOOT 都没有，永远不会走到这条路径，暴露面为 0。
 *   2. 不注入任何 .so 到游戏进程 —— /proc/<pid>/maps 与游戏进程完全干净。
 *   3. 绘制不新增 SurfaceControl 层 —— 直接写 DRM scanout buffer，
 *      SurfaceFlinger 层枚举无异常项。
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

#ifdef __KERNEL__
#ifndef f32
typedef float f32;
#endif
#endif

#define HX_VERSION      0x03010000u
#define HX_ABI          3

/* ------------------------------------------------------------------ *
 *  控制通道（__NR_reboot 后门）
 *  reboot(magic1, magic2, cmd, arg)
 *  magic 与 TwT(0x114514/0x1919810) 无任何重叠，也不在公开样本特征库里
 * ------------------------------------------------------------------ */
#define HX_GATE_M1      0x484C5831u   /* "HLX1" */
#define HX_GATE_M2      0x5A17C0DEu

/*
 * hx_gate 必须 <= 128 字节，放在用户栈上传递即可。
 * key 做二次校验，防止别人用同样 magic 暴力探测出后门存在。
 *   key = ((~cmd) * 0x9E3779B1u) ^ 0xA5C39E71u
 */
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
	HX_CMD_PING     = 1,   /* out0 = HX_VERSION, out1 = uptime ms */
	HX_CMD_PID      = 2,   /* arg0 = 用户态 char* 进程名(<=15B), out0 = pid */
	HX_CMD_MODULE   = 3,   /* arg0=pid, arg1=用户态 char* so 名, out0=base, out1=end */
	HX_CMD_READ     = 4,   /* arg0=pid, arg1=addr, arg2=buf|len<<48  见 hx_rw_pack */
	HX_CMD_WRITE    = 5,
	HX_CMD_STAT     = 6,   /* out0 = 采样次数, out1 = 上次耗时 us */
	HX_CMD_CFG      = 7,   /* arg0 = 用户态 struct hx_cfg* */
	HX_CMD_UNBIND   = 8,   /* 清空 gate 会话，回到纯内嵌配置模式 */
};

/* HX_CMD_READ/WRITE 的打包方式：len 放在 arg2 高 16 位，缓冲指针低 48 位 */
#define hx_rw_pack(ptr, len)  ((((u64)(len) & 0xFFFFull) << 48) | \
			       ((u64)(ptr) & 0x0000FFFFFFFFFFFFull))
#define hx_rw_ptr(p)          ((u64)(p) & 0x0000FFFFFFFFFFFFull)
#define hx_rw_len(p)          ((u32)(((u64)(p) >> 48) & 0xFFFFu))

/* ------------------------------------------------------------------ *
 *  运行时配置（hxctl cfg 下发；也可编译期内嵌）
 *  offset 全部可覆盖 —— 游戏更新只改这一张表，不用重编内核
 * ------------------------------------------------------------------ */
#define HX_MAX_SLOT  16

struct hx_layer {
	u32 on;            /* 层开关 */
	u32 color;         /* 0xAARRGGBB */
	s32 thick;
	s32 pad;
};

struct hx_cfg {
	u32 abi;
	u32 flags;         /* HX_FLAG_* */
	char pkg[32];      /* com.tencent.tmgp.sgame */
	char core_so[24];  /* libGameCore.so */
	char world_so[24]; /* libil2cpp.so（部分版本锚点在它身上） */

	/* ---- 锚点与结构偏移（默认值来自本机实测逆向，见 docs/offset.md） ---- */
	u64 an_off;        /* 段尾 + an_off -> 战斗管理器；默认 0x182810 */
	u32 o_mgr_ent;     /* 管理器 + o_mgr_ent -> 实体管理器；0x00 */
	u32 o_ent_head;    /* 实体管理器 + o_ent_head -> 链表头；0x1D0 */
	u32 o_ent_next;    /* 实体 + o_ent_next；0x150 */
	u32 o_ent_actor;   /* 实体 + o_ent_actor -> Actor；0xF8 */
	u32 o_actor_x;     /* Actor + o_actor_x -> float x；0x3C */
	u32 o_actor_z;     /* Actor + o_actor_z -> float z；0x40 */
	u32 o_actor_hp;    /* Actor + o_actor_hp -> int hp（0=关） */
	u32 o_actor_team;  /* Actor + o_actor_team -> int 阵营（0=关） */
	u32 o_actor_camp;  /* Actor + o_actor_camp -> int camp/type */
	u32 ent_max;       /* 单次最多走多少个实体；0=16 */
	u32 hud_w;         /* 屏幕像素，0=自动从 DRM 模式取 */
	u32 hud_h;

	/* ---- 世界->屏幕（透视用，0 表示只开雷达） ---- */
	u64 w2s_addr;      /* 游戏内 view-proj 矩阵地址，0=雷达模式 */
	u32 w2s_stride;
	u32 w2s_rows;

	/* ---- 雷达（小地图）参数 ---- */
	s32 radar_cx, radar_cy;   /* 雷达圆心（像素） */
	s32 radar_r;              /* 半径（像素） */
	f32 world_span;           /* 世界半宽（米），默认 200 */
	f32 radar_rot_deg;

	/* ---- 自瞄 ---- */
	u32 aim_on;
	u32 aim_key;              /* 触发方式位掩码，见 HX_AIM_* */
	s32 aim_fov;
	s32 aim_smooth;           /* 0..100 */
	s32 aim_lead_ms;
	s32 aim_slot_px;          /* 落点半径抖动，抗行为检测 */

	/* ---- 绘制层 ---- */
	struct hx_layer ly_box;     /* 方框 */
	struct hx_layer ly_line;    /* 射线 */
	struct hx_layer ly_hp;      /* 血条 */
	struct hx_layer ly_radar;   /* 雷达点 */
	struct hx_layer ly_text;    /* 文本 */

	u32 tick_ms;        /* 采样间隔，默认 16（内核态代价极低） */
	u32 max_read;       /* 单次读上限，默认 0x10000 */
};

enum {
	HX_FLAG_LOOSE_CLOCK = 1u << 0,  /* 注入事件时间戳贴合真实节奏 */
	HX_FLAG_HIDE_SELF   = 1u << 1,  /* 摘 modules 链（默认开） */
	HX_FLAG_NO_LOG      = 1u << 2,  /* 全程不 printk */
	HX_FLAG_CAMP_FILTER = 1u << 3,
	HX_FLAG_DRM_ALPHA   = 1u << 4,  /* 送显前混合，而非覆盖 */
};

enum { HX_AIM_ALWAYS = 1, HX_AIM_TOUCH = 2, HX_AIM_KEY = 4 };

#define HX_READ_CHUNK  (4096ull)
#define HX_MAX_ENT     64

/* 内核内部用的实体快照（不跨边界） */
struct hx_ent {
	u64 actor;
	float x, z;
	s32 hp, team;
};

#endif /* _HX_H */
