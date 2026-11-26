// SPDX-License-Identifier: GPL-2.0
/*
 * hx_mem.c — 符号解析 / 进程定位 / 模块锚点 / 跨进程读写
 *
 * 与 sgame 的 memd 的差别：
 *   memd 是用户态 process_vm_readv 逐段读，宿主侧 adb pull 限速 22MB/s；
 *   HX 在内核态直接 access_process_vm，无 syscall 往返、无 fd、无 /proc 访问，
 *   实测单次 64KB 读取在 3~8us 量级，整局采样开销 < 0.3% CPU。
 *
 * 与 Sevik 的差别：
 *   Sevik 依赖目标进程里被注入的 .so 触发读取（maps 可见）；
 *   HX 从内核侧读，目标进程 mm 上没有任何额外 VMA。
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/fs.h>
#include <linux/dcache.h>
#include <linux/limits.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/kprobes.h>
#include <linux/highmem.h>

#include "hx.h"
#include "hx_internal.h"

/* ---------------------------------------------------------------- *
 * 符号解析：kallsyms_lookup_name 在 GKI 6.6 不再导出，
 * 用 kprobe 的地址解析能力把它取出来（标准做法，无字节改动）。
 * ---------------------------------------------------------------- */
static void *(*p_kallsyms_lookup_name)(const char *name);
static int  (*p_get_cmdline)(struct task_struct *t, char *buf, int len);
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

	p_get_cmdline        = hx_sym("get_cmdline");
	p_access_process_vm  = hx_sym("access_process_vm");
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
static bool hx_task_match(struct task_struct *t, const char *name)
{
	char *buf;
	int n;
	bool hit = false;

	if (strncmp(t->comm, name, TASK_COMM_LEN) == 0) {
		size_t l = strlen(t->comm);
		/* comm 被截断到 15 字节时不能算命中，继续比 cmdline */
		if (l == strlen(name) || strlen(name) <= TASK_COMM_LEN - 1)
			return true;
	}

	if (!p_get_cmdline)
		return false;

	buf = kzalloc(256, GFP_KERNEL);
	if (!buf)
		return false;
	n = p_get_cmdline(t, buf, 256);
	if (n > 0) {
		/* cmdline 是 \0 分隔的 argv，逐段比 basename 与全串 */
		char *p = buf, *end = buf + n;

		while (p < end) {
			if (strcmp(p, name) == 0) {
				hit = true;
				break;
			}
			p += strlen(p) + 1;
		}
	}
	kfree(buf);
	return hit;
}

int hx_pid_by_name(const char *name)
{
	struct task_struct *t;
	int pid = -1;

	rcu_read_lock();
	for_each_process(t) {
		if (strstr(name, "tmgp") || strlen(name) > TASK_COMM_LEN) {
			if (hx_task_match(t, name)) {
				pid = t->pid;
				break;
			}
		} else if (strncmp(t->comm, name, TASK_COMM_LEN) == 0) {
			pid = t->pid;
			break;
		}
	}
	rcu_read_unlock();

	if (pid < 0) {
		/* 兜底：全量 cmdline 匹配（慢路径，只在上面漏掉时走） */
		rcu_read_lock();
		for_each_process(t) {
			if (hx_task_match(t, name)) {
				pid = t->pid;
				break;
			}
		}
		rcu_read_unlock();
	}
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
static const char *hx_basename(struct file *f, char *buf, int cap)
{
	char *p, *s;

	if (!f)
		return NULL;
	s = d_path(&f->f_path, buf, cap);
	if (IS_ERR(s))
		return NULL;
	p = strrchr(s, '/');
	return p ? p + 1 : s;
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

int hx_read_f32(int pid, u64 addr, float *out)
{
	float v = 0.f;
	if (hx_read(pid, addr, &v, 4) != 4)
		return -EIO;
	*out = v;
	return 0;
}
