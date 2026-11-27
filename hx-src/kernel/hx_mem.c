// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/kallsyms.h>
#include <linux/kprobes.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/pid.h>
#include <linux/dcache.h>
#include <linux/path.h>

#include "hx.h"
#include "hx_internal.h"

/* ---------------------------------------------------------------- *
 * 符号解析
 *
 * kallsyms_lookup_name 在 GKI 6.6 没有 EXPORT_SYMBOL，模块里直接调会
 * undefined。标准做法是用 kprobe 的符号解析能力把地址取出来：
 * 注册一个 symbol_name 指向它的空 kprobe，kp.addr 就是函数地址。
 * ---------------------------------------------------------------- */
static void *(*p_kallsyms_lookup_name)(const char *name);
static int  (*p_access_process_vm)(struct task_struct *tsk, unsigned long addr,
				   void *buf, int len, unsigned int gup_flags);
static long (*p_get_user_pages_remote)(struct mm_struct *mm, unsigned long start,
				       unsigned long nr, unsigned int flags,
				       struct page **pages, int *locked);

int hx_sym_init(void)
{
	static struct kprobe kp = { .symbol_name = "kallsyms_lookup_name" };

	if (register_kprobe(&kp))
		return -ENOENT;

	p_kallsyms_lookup_name = (void *)kp.addr;
	unregister_kprobe(&kp);

	if (!p_kallsyms_lookup_name)
		return -ENOENT;

	p_access_process_vm     = hx_sym("access_process_vm");
	p_get_user_pages_remote = hx_sym("get_user_pages_remote");
	return 0;
}

void *hx_sym(const char *name)
{
	if (!p_kallsyms_lookup_name)
		return NULL;
	return (void *)p_kallsyms_lookup_name(name);
}

/* ---------------------------------------------------------------- *
 * 进程定位
 * ---------------------------------------------------------------- */
int hx_pid_by_name(const char *name)
{
	struct task_struct *t;
	int pid = -1;
	size_t n = strlen(name);

	/* comm 只存 15 个字符，后面的会被截断 */
	if (n > TASK_COMM_LEN - 1)
		n = TASK_COMM_LEN - 1;

	/*
	 * 这里绝不能调 get_cmdline()：它内部 mmput()/down_read()，会睡眠，
	 * 而本循环跑在 rcu_read_lock() 里 —— 睡眠立刻触发
	 * "sleeping in RCU read side critical section" 然后 panic。
	 * 实测 com.tencent.tmgp.sgame 的 comm 是 com.tencent.tmg，
	 * 前缀匹配足够唯一定位。
	 */
	rcu_read_lock();
	for_each_process(t) {
		if (strncmp(t->comm, name, n) == 0) {
			pid = t->pid;
			break;
		}
	}
	rcu_read_unlock();

	g_stat.last_pid = pid;
	return pid;
}

static struct task_struct *hx_task(int pid)
{
	struct task_struct *t;

	if (pid <= 0)
		return NULL;
	rcu_read_lock();
	t = find_task_by_vpid(pid);
	if (t)
		get_task_struct(t);
	rcu_read_unlock();
	return t;
}

/* ---------------------------------------------------------------- *
 * 模块锚点：找 so 的 VMA 集合，返回最小基址与"最后一个 rw 段的结束地址"
 * HoK 的锚点是 libGameCore.so 最后一个 rw- 段的 end + 0x182810
 * ---------------------------------------------------------------- */
/*
 * 取 VMA 对应的文件名。
 *
 * 不能用 d_path()：它要拿 rename_lock/mount_lock，而本函数是在持有
 * mmap_read_lock 的情况下被调用的，两者嵌套会有锁序问题 —— 表现为开机后
 * 采样线程跑到这里就卡死，看起来和 bootloop 一样。
 * 直接读 dentry 的 d_name 就够了：vma 持有 file 引用，dentry 必然有效。
 */
static const char *hx_basename(struct file *f, char *buf, int cap)
{
	if (!f || !f->f_path.dentry)
		return NULL;
	(void)buf; (void)cap;
	return (const char *)f->f_path.dentry->d_name.name;
}

static int hx_module_range_impl(int pid, const char *so, u64 *base, u64 *end,
				bool *rw_end, u64 *rw_lo)
{
	struct task_struct *t = hx_task(pid);
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	char *pbuf;
	u64 lo = ~0ull, hi = 0, last_rw = 0, rw_best = 0;
	int found = 0;

	if (!t)
		return -ENOENT;

	mm = get_task_mm(t);
	put_task_struct(t);
	if (!mm)
		return -ENOENT;

	pbuf = kzalloc(PATH_MAX, GFP_KERNEL);
	if (!pbuf) {
		mmput(mm);
		return -ENOMEM;
	}

	mmap_read_lock(mm);
	VMA_ITERATOR(vmi, mm, 0);
	for_each_vma(vmi, vma) {
		const char *bn;

		if (!vma->vm_file)
			continue;
		bn = hx_basename(vma->vm_file, pbuf, PATH_MAX);
		if (!bn || strcmp(bn, so) != 0)
			continue;

		found = 1;
		if (vma->vm_start < lo)
			lo = vma->vm_start;
		if (vma->vm_end > hi)
			hi = vma->vm_end;
		/* 记录最后一个可写段的尾部（VMA 按地址升序遍历） */
		if ((vma->vm_flags & VM_WRITE) && vma->vm_end > last_rw) {
			last_rw = vma->vm_end;
			rw_best = vma->vm_start;
		}
	}
	mmap_read_unlock(mm);
	kfree(pbuf);
	mmput(mm);

	if (!found)
		return -ENOENT;
	if (base)
		*base = lo;
	if (end)
		*end = hi;
	if (rw_end)
		*rw_end = !!last_rw;
	if (rw_lo)
		*rw_lo = rw_best;
	/* end 语义：给锚点用时应传最后一个可写段尾 */
	if (last_rw)
		*end = last_rw;
	return 0;
}

int hx_module_range(int pid, const char *so, u64 *base, u64 *end)
{
	return hx_module_range_impl(pid, so, base, end, NULL, NULL);
}

int hx_module_range_any(int pid, const char *so, u64 *base, u64 *end, bool *rw)
{
	u64 rwlo = 0;
	return hx_module_range_impl(pid, so, base, end, rw, &rwlo);
}

/* ---------------------------------------------------------------- *
 * 跨进程读写
 * ---------------------------------------------------------------- */
static inline u64 hx_untag(u64 a)
{
	/* ARM MTE：0xb4xxxxxxxxxxxxxx -> 0x00xxxxxxxxxxxxxx */
	return a & 0x00FFFFFFFFFFFFFFull;
}

static int hx_read_gup(struct task_struct *t, u64 addr, void *buf, u32 len)
{
	struct mm_struct *mm = get_task_mm(t);
	struct page *pg;
	int locked = 0;
	long got;
	u32 done = 0;

	if (!mm || !p_get_user_pages_remote)
		goto out;

	while (done < len) {
		u64 cur = addr + done;
		u32 off = cur & (PAGE_SIZE - 1);
		u32 chunk = min_t(u32, len - done, PAGE_SIZE - off);

		locked = 0;
		got = p_get_user_pages_remote(mm, cur & PAGE_MASK, 1,
					      FOLL_FORCE, &pg, &locked);
		if (got != 1)
			break;
		{
			void *kaddr = kmap_local_page(pg);

			memcpy((u8 *)buf + done, (u8 *)kaddr + off, chunk);
			kunmap_local(kaddr);
		}
		put_page(pg);
		done += chunk;
	}
out:
	if (mm)
		mmput(mm);
	return done;
}

int hx_read(int pid, u64 addr, void *buf, u32 len)
{
	struct task_struct *t;
	int n = -EIO;

	t = hx_task(pid);
	if (!t)
		return -ESRCH;
	addr = hx_untag(addr);

	if (p_access_process_vm) {
		n = p_access_process_vm(t, addr, buf, (int)len, FOLL_FORCE);
		if (n < 0)
			n = -EIO;
	}
	if (n <= 0)
		n = hx_read_gup(t, addr, buf, len);

	put_task_struct(t);
	if (n > 0)
		g_stat.read_bytes += n;
	return n;
}

int hx_write(int pid, u64 addr, const void *buf, u32 len)
{
	struct task_struct *t = hx_task(pid);
	int n = -ESRCH;

	if (!t)
		return -ESRCH;
	addr = hx_untag(addr);
	if (p_access_process_vm)
		n = p_access_process_vm(t, addr, (void *)buf, (int)len,
					FOLL_FORCE | FOLL_WRITE);
	put_task_struct(t);
	return n;
}

int hx_read_u64(int pid, u64 addr, u64 *out)
{
	u64 v = 0;
	if (hx_read(pid, addr, &v, 8) != 8)
		return -EIO;
	*out = hx_untag(v);
	return 0;
}

int hx_read_u32(int pid, u64 addr, u32 *out)
{
	u32 v = 0;
	if (hx_read(pid, addr, &v, 4) != 4)
		return -EIO;
	*out = v;
	return 0;
}

/*
 * IEEE754 单精度位模式 -> Q16.16 定点，纯整数运算。
 * 内核禁用浮点，绝不能写成 (fx_t)(float)v —— 那会引用 __fixsfsi。
 * 1.0f 的位模式 0x3F800000 换算后恰好等于 FX_ONE(65536)。
 */
fx_t hx_f32_to_fx(u32 bits)
{
	s32 sign = (s32)(bits >> 31);
	s32 exp = (s32)((bits >> 23) & 0xFF);
	u32 mant = (bits & 0x7FFFFFu) | 0x800000u;
	s32 sh, v;

	if (exp == 0)        /* 0 或 denormal */
		return 0;
	if (exp == 0xFF)     /* inf / nan */
		return 0;

	sh = exp - 127 - (23 - FX_SHIFT);
	if (sh >= 0)
		v = (sh > 15) ? 0x7FFFFFFF : (s32)(mant << sh);
	else {
		sh = -sh;
		v = (sh > 31) ? 0 : (s32)(mant >> sh);
	}
	return sign ? -v : v;
}

int hx_read_fx(int pid, u64 addr, fx_t *out)
{
	u32 bits = 0;

	if (hx_read(pid, addr, &bits, 4) != 4)
		return -EIO;
	*out = hx_f32_to_fx(bits);
	return 0;
}
