// SPDX-License-Identifier: GPL-2.0
/*
 * hx_scan.c — 实体采集
 *
 * 数据链（来自本机实测逆向，见 docs/offset.md）：
 *   libGameCore.so 最后一个 rw- 段尾 + an_off(0x182810) -> 战斗管理器
 *   管理器 + 0x00      -> 实体管理器
 *   实体管理器 + 0x1D0 -> 链表头
 *   实体 + 0x150       -> next
 *   实体 + 0xF8        -> Actor
 *   Actor + 0x3C/0x40  -> float x / z（读成 Q16.16 定点）
 *
 * 与 sgame 的做法差异：
 *   sgame 每次采样都要 memd 逐段读 + 宿主 Python 解析，一轮 3.2GB/2s；
 *   HX 只用 3~4 次 8 字节读取走完链表，单轮 < 20us，可以 60Hz 采样。
 *   并且 HX 在链表走空时会自动用"属性数组 stride 0x84"做二次校验，
 *   避免游戏更新后 Actor 偏移漂移导致整局无数据。
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/jiffies.h>

#include "hx.h"
#include "hx_internal.h"

/* 属性数组兜底校验用：stride 两套候选（README 0x78 / 逆向 0x84） */
static const u32 hx_attr_stride[] = { 0x84, 0x78 };

static int hx_walk(int pid, u64 head, struct hx_ent *out, int cap)
{
	u64 cur = head;
	int n = 0;
	u64 seen[HX_MAX_ENT] = { 0 };

	if (!head || head < 0x1000)
		return 0;

	while (cur && n < cap) {
		u64 actor = 0, next = 0;
		struct hx_ent e = { 0 };
		int i, dup = 0;

		/* 防环：链表被人为构造时不会把我们卡死在内核里 */
		for (i = 0; i < n; i++) {
			if (seen[i] == cur) {
				dup = 1;
				break;
			}
		}
		if (dup)
			break;
		seen[n] = cur;

		if (hx_read_u64(pid, cur + g_cfg.o_ent_actor, &actor) || !actor)
			break;

		e.actor = actor;
		hx_read_fx(pid, actor + g_cfg.o_actor_x, &e.x);
		hx_read_fx(pid, actor + g_cfg.o_actor_z, &e.z);
		if (g_cfg.o_actor_hp)
			hx_read_u32(pid, actor + g_cfg.o_actor_hp, (u32 *)&e.hp);
		if (g_cfg.o_actor_team)
			hx_read_u32(pid, actor + g_cfg.o_actor_team, (u32 *)&e.team);

		/* 合理性过滤：坐标必须是地图量级，否则说明 offset 漂了 */
		if (e.x > FX_INT(-4000) && e.x < FX_INT(4000) &&
		    e.z > FX_INT(-4000) && e.z < FX_INT(4000) && (e.x || e.z))
			out[n++] = e;

		if (hx_read_u64(pid, cur + g_cfg.o_ent_next, &next))
			break;
		if (next == cur)
			break;
		cur = next;
	}
	return n;
}

/*
 * 兜底：直接扫属性数组（stride 0x84）定位"类型标记=1 + 物攻/护甲/血量合理"的块，
 * 再回推 0x60 的 owner 指针拿 Actor。只在链表路径完全失败时启用（每 2s 一次）。
 */
static int hx_scan_attr_fallback(int pid, u64 bss_lo, u64 bss_hi,
				 struct hx_ent *out, int cap)
{
	u8 *buf;
	u64 addr;
	int n = 0;

	if (bss_hi - bss_lo > (64ull << 20))
		bss_hi = bss_lo + (64ull << 20);

	buf = kmalloc(HX_READ_CHUNK, GFP_KERNEL);
	if (!buf)
		return 0;

	for (addr = bss_lo; addr + HX_READ_CHUNK <= bss_hi && n < cap;
	     addr += HX_READ_CHUNK) {
		int r = hx_read(pid, addr, buf, HX_READ_CHUNK);
		u32 s;

		if (r <= 0)
			break;
		for (s = 0; s < (u32)r / 4; s++) {
			u32 tag, atk, arm, hpmax, hpnow;
			int k;

			for (k = 0; k < ARRAY_SIZE(hx_attr_stride); k++) {
				u32 stride = hx_attr_stride[k];
				u32 off = s * 4;

				if (off + stride > (u32)r)
					continue;
				tag = *(u32 *)(buf + off);
				if (tag != 1)
					continue;
				atk   = *(u32 *)(buf + off + 4);
				arm   = *(u32 *)(buf + off + 0x0C);
				hpmax = *(u32 *)(buf + off + 0x10);
				hpnow = *(u32 *)(buf + off + 0x14);
				if (atk < 50 || atk > 900)
					continue;
				if (arm > 900)
					continue;
				if (hpmax < 300 || hpmax > 20000)
					continue;
				if (hpnow > hpmax + 100)
					continue;
				/* 命中一块属性，用块内 0x60 的 owner 反推 Actor */
				if (off + 0x68 <= (u32)r) {
					u64 owner = *(u64 *)(buf + off + 0x60);
					struct hx_ent e = { 0 };

					if (owner < 0x1000)
						continue;
					e.actor = owner;
					hx_read_fx(pid, owner + g_cfg.o_actor_x, &e.x);
					hx_read_fx(pid, owner + g_cfg.o_actor_z, &e.z);
					e.hp = (s32)hpnow;
					if (e.x > FX_INT(-4000) && e.x < FX_INT(4000) &&
					    e.z > FX_INT(-4000) && e.z < FX_INT(4000))
						out[n++] = e;
					break;
				}
			}
			if (n >= cap)
				break;
		}
	}
	kfree(buf);
	return n;
}

int hx_scan_once(struct hx_ent *out, int cap)
{
	int pid = hx_pid_by_name(g_cfg.pkg);
	u64 base = 0, end = 0, mgr = 0, ent_mgr = 0, head = 0;
	int n = 0;
	u32 want;

	if (pid <= 0)
		return 0;

	if (hx_module_range(pid, g_cfg.core_so, &base, &end))
		return 0;

	/* 锚点：最后可写段尾 + 偏移 */
	if (hx_read_u64(pid, end + g_cfg.an_off, &mgr) || !mgr)
		goto fallback;

	if (hx_read_u64(pid, mgr + g_cfg.o_mgr_ent, &ent_mgr) || !ent_mgr)
		goto fallback;

	if (hx_read_u64(pid, ent_mgr + g_cfg.o_ent_head, &head) || !head)
		goto fallback;

	want = g_cfg.ent_max ? min_t(u32, g_cfg.ent_max, HX_MAX_ENT) : 16;
	n = hx_walk(pid, head, out, min(cap, (int)want));
	if (n > 0)
		return n;

fallback:
	/* 每 2s 才允许做一次全段扫描，避免占 CPU */
	{
		static unsigned long last;
		bool rw = false;

		if (!time_after(jiffies, last + msecs_to_jiffies(2000)))
			return 0;
		last = jiffies;
		if (hx_module_range_any(pid, g_cfg.core_so, &base, &end, &rw))
			return 0;
		return hx_scan_attr_fallback(pid, base, end, out, cap);
	}
}

/* 自己的坐标：链表首个实体即本地玩家（实测 10 个实体里 index 0 是自己） */
int hx_self_pos(fx_t *x, fx_t *z)
{
	struct hx_ent e[HX_MAX_ENT];
	int n = hx_scan_once(e, HX_MAX_ENT);

	if (n <= 0)
		return -ENODATA;
	*x = e[0].x;
	*z = e[0].z;
	return 0;
}
