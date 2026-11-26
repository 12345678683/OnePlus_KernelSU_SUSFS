// SPDX-License-Identifier: GPL-2.0
/*
 * hx_gate.c — 无节点控制通道
 *
 * 原理：kprobe 挂在 __arm64_sys_reboot 入口，pre_handler 里校验
 *       (magic1, magic2, key)。命中则改写 x0 并返回 1（跳过原函数），
 *       未命中返回 0，走真实 reboot 逻辑。
 *
 * 为什么比 TwT 那条路更干净：
 *   - TwT 是在 __arm64_sys_reboot 里加了分支（或 inline hook），改的是
 *     内核代码段字节；HX 用的是 kprobe（ftrace 机制）本身合法，
 *     且不改任何指令字节。
 *   - kprobe 命中前没有任何 fd、没有 /dev 节点、没有 netlink socket，
 *     ACE 在 app 域既看不到也调不到（缺 CAP_SYS_BOOT）。
 *   - 会话状态（bound）默认关闭；不 bind 时整个后门等于不存在。
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/jiffies.h>
#include <linux/version.h>

#include "hx.h"
#include "hx_internal.h"

#define HX_GATE_KEY 0xA5C39E71u

struct hx_gate_state {
	bool  bound;
	u64   bind_jiffies;
	u64   calls;
} g_gate;

static struct kprobe hx_reboot_kp = {
	.symbol_name = "__arm64_sys_reboot",
};

static inline u32 hx_key_of(u32 cmd)
{
	return ((~cmd) * 0x9E3779B1u) ^ HX_GATE_KEY;
}

/* 用户指针安全取串。返回值 <= 0 表示失败。 */
static int hx_str_from_user(u64 uptr, char *dst, int cap)
{
	int n;

	if (!uptr || !cap)
		return -EINVAL;
	n = strncpy_from_user(dst, (const char __user *)(uintptr_t)uptr, cap - 1);
	if (n < 0)
		return (int)n;
	dst[n] = 0;
	return n;
}

static long hx_dispatch(struct hx_gate *g, struct pt_regs *regs)
{
	long ret = -EINVAL;

	switch (g->cmd) {
	case HX_CMD_PING:
		g->out0 = HX_VERSION;
		g->out1 = (u64)jiffies_to_msecs(jiffies);
		ret = 0;
		break;

	case HX_CMD_PID: {
		char name[32];
		if (hx_str_from_user(g->arg0, name, sizeof(name)) <= 0)
			break;
		g->out0 = (u64)(s64)hx_pid_by_name(name);
		ret = (long)g->out0;
		break;
	}

	case HX_CMD_MODULE: {
		char name[40];
		u64 base = 0, end = 0;
		if (hx_str_from_user(g->arg1, name, sizeof(name)) <= 0)
			break;
		if (hx_module_range((int)g->arg0, name, &base, &end) == 0) {
			g->out0 = base;
			g->out1 = end;
			ret = 0;
		}
		break;
	}

	case HX_CMD_READ: {
		u64 dst = hx_rw_ptr(g->arg2);
		u32 len = hx_rw_len(g->arg2);
		void *kbuf;

		if (!len || len > g_cfg.max_read)
			break;
		kbuf = kzalloc(len, GFP_KERNEL);
		if (!kbuf)
			break;
		if (hx_read((int)g->arg0, g->arg1, kbuf, len) == len) {
			if (copy_to_user((void __user *)(uintptr_t)dst, kbuf, len) == 0)
				ret = 0;
		}
		kfree(kbuf);
		break;
	}

	case HX_CMD_WRITE: {
		u64 src = hx_rw_ptr(g->arg2);
		u32 len = hx_rw_len(g->arg2);
		void *kbuf;

		if (!len || len > g_cfg.max_read)
			break;
		kbuf = kzalloc(len, GFP_KERNEL);
		if (!kbuf)
			break;
		if (copy_from_user(kbuf, (const void __user *)(uintptr_t)src, len) == 0) {
			if (hx_write((int)g->arg0, g->arg1, kbuf, len) == len)
				ret = 0;
		}
		kfree(kbuf);
		break;
	}

	case HX_CMD_STAT:
		g->out0 = g_stat.ticks;
		g->out1 = g_stat.last_us;
		ret = 0;
		break;

	case HX_CMD_CFG: {
		struct hx_cfg cfg;
		if (copy_from_user(&cfg, (const void __user *)(uintptr_t)g->arg0,
				   sizeof(cfg)))
			break;
		if (cfg.abi != HX_ABI)
			break;
		hx_cfg_apply(&cfg);
		ret = 0;
		break;
	}

	case HX_CMD_UNBIND:
		g_gate.bound = false;
		hx_feature_suspend();
		ret = 0;
		break;

	case HX_CMD_NOP:
		ret = 0;
		break;
	}

	regs->regs[0] = (u64)ret;
	return ret;
}

static int hx_reboot_pre(struct kprobe *kp, struct pt_regs *regs)
{
	struct hx_gate g;
	u32 m1, m2, cmd;
	u64 uarg;

	m1 = (u32)regs->regs[0];
	if (m1 != HX_GATE_M1)
		return 0;
	m2 = (u32)regs->regs[1];
	if (m2 != HX_GATE_M2)
		return 0;

	cmd = (u32)regs->regs[2];
	uarg = regs->regs[3];
	if (!uarg)
		return 0;

	if (copy_from_user(&g, (const void __user *)(uintptr_t)uarg, sizeof(g)))
		return 0;

	if (g.magic != HX_GATE_M1 || g.version != HX_ABI || g.cmd != cmd)
		return 0;
	if (g.key != hx_key_of(cmd))
		return 0;

	g_gate.calls++;
	g_gate.bound = true;
	g_gate.bind_jiffies = jiffies;

	hx_dispatch(&g, regs);

	if (copy_to_user((void __user *)(uintptr_t)uarg, &g, sizeof(g)))
		regs->regs[0] = (u64)(long)-EFAULT;

	return 1;   /* 跳过真正的 reboot */
}

int hx_gate_init(void)
{
	int r;

	memset(&g_gate, 0, sizeof(g_gate));
	r = register_kprobe(&hx_reboot_kp);
	if (r) {
		/* 拿不到 kprobe 不致命：退回纯内嵌配置模式 */
		hx_warn("gate: kprobe register failed %d, embedded mode only\n", r);
		return r;
	}
	return 0;
}

void hx_gate_exit(void)
{
	unregister_kprobe(&hx_reboot_kp);
	g_gate.bound = false;
}
