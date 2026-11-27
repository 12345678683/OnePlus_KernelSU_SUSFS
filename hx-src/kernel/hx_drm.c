// SPDX-License-Identifier: GPL-2.0
/*
 * hx_drm.c — 送显前直绘（ESP）
 *
 * 这是 HX 相对 Sevik / sgame 最大的结构性差异：
 *
 *   Sevik：EGL/GLES + SurfaceComposerClient 新建一个 SurfaceControl 层，
 *          叠在游戏画面上。SurfaceFlinger 的层列表里多一条，
 *          `dumpsys SurfaceFlinger --list` 与截屏取证都能抓到。
 *   sgame：根本没有绘制，只有文本输出。
 *   HX   ：挂在 drm_atomic_commit 上，拿到这一帧即将送显的 framebuffer，
 *          通过 dma_buf 的 CPU 映射直接写像素。合成器视角里
 *          层数、层名、层属性全部没变，画面里却多了方框 ——
 *          也就是"像素级存在、结构级不存在"。
 *
 * 时序约束（重要）：
 *   - 必须在本帧 GPU 渲染完成之后、DPU 取走之前写入。
 *     drm_atomic_commit 的入口正好落在这个窗口内（msm 的 commit_tail 会把
 *     fb 交给 DPU 的 atomic_update，之后 CPU 仍有一次 cache 可见窗口）。
 *   - 写之前/之后要 begin/end_cpu_access，否则 GPU 的 cache 会把我们的
 *     写覆盖掉（高通平台普遍是 IO-coherent，仍需 flush）。
 *   - kprobe 上下文不能睡眠：所以 vmap 只在 workqueue 里做一次并缓存，
 *     之后 pre_handler 里是纯 memcpy，耗时 < 1ms。
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/slab.h>
#include <linux/dma-buf.h>
#include <linux/delay.h>
#include <linux/workqueue.h>

#include <drm/drm_device.h>
#include <drm/drm_crtc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_atomic.h>
#include <drm/drm_plane.h>

#include "hx.h"
#include "hx_internal.h"

struct hx_fb_cache {
	struct drm_framebuffer *fb;
	void   *vaddr;
	u32     pitch;
	u32     cpp;
	u32     w, h;
	u32     format;
	bool    valid;
};

static struct hx_fb_cache g_fbc;
static struct drm_device  *g_ddev;
static struct kprobe       g_kp_commit;
static struct work_struct  g_map_work;
static struct hx_ent       g_last[HX_MAX_ENT];
static int                 g_last_n;
static spinlock_t          g_lock;

bool hx_drm_ready(void)
{
	return g_fbc.valid;
}

static inline u32 hx_px(struct hx_fb_cache *c, int x, int y)
{
	if (x < 0 || y < 0 || (u32)x >= c->w || (u32)y >= c->h)
		return 0;
	if (c->cpp == 4)
		return *(u32 *)((u8 *)c->vaddr + (size_t)y * c->pitch + (size_t)x * 4);
	if (c->cpp == 2)
		return *(u16 *)((u8 *)c->vaddr + (size_t)y * c->pitch + (size_t)x * 2);
	return 0;
}

static inline void hx_put_px(struct hx_fb_cache *c, int x, int y, u32 argb)
{
	u8 *p;

	if (x < 0 || y < 0 || (u32)x >= c->w || (u32)y >= c->h)
		return;
	p = (u8 *)c->vaddr + (size_t)y * c->pitch + (size_t)x * c->cpp;
	if (c->cpp == 4) {
		u32 old = *(u32 *)p;
		if (g_cfg.flags & HX_FLAG_DRM_ALPHA) {
			u32 a = (argb >> 24) & 0xFF;
			u32 ia = 255 - a;
			u32 r = (((argb >> 16) & 0xFF) * a + ((old >> 16) & 0xFF) * ia) / 255;
			u32 g = (((argb >> 8) & 0xFF) * a + ((old >> 8) & 0xFF) * ia) / 255;
			u32 b = ((argb & 0xFF) * a + (old & 0xFF) * ia) / 255;
			*(u32 *)p = (old & 0xFF000000u) | (r << 16) | (g << 8) | b;
		} else {
			*(u32 *)p = argb;
		}
	} else if (c->cpp == 2) {
		/* RGB565 */
		u16 v = (u16)(((argb >> 8) & 0xF800) | ((argb >> 5) & 0x07E0) |
			      ((argb >> 3) & 0x001F));
		*(u16 *)p = v;
	}
}

static void hx_line(struct hx_fb_cache *c, int x0, int y0, int x1, int y1,
		    u32 color, int thick)
{
	int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
	int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
	int err = dx + dy, t, guard = 0;

	for (;;) {
		for (t = 0; t < thick; t++)
			hx_put_px(c, x0 + t, y0, color);
		if (x0 == x1 && y0 == y1)
			break;
		{
			int e2 = 2 * err;

			if (e2 >= dy) { err += dy; x0 += sx; }
			if (e2 <= dx) { err += dx; y0 += sy; }
		}
		if (++guard > 8192)
			break;
	}
}

static void hx_rect(struct hx_fb_cache *c, int x, int y, int w, int h,
		    u32 color, int thick)
{
	hx_line(c, x, y, x + w, y, color, thick);
	hx_line(c, x, y + h, x + w, y + h, color, thick);
	hx_line(c, x, y, x, y + h, color, thick);
	hx_line(c, x + w, y, x + w, y + h, color, thick);
}

static void hx_dot(struct hx_fb_cache *c, int x, int y, int r, u32 color)
{
	int i, j;

	for (i = -r; i <= r; i++)
		for (j = -r; j <= r; j++)
			if (i * i + j * j <= r * r)
				hx_put_px(c, x + i, y + j, color);
}

/*
 * 世界坐标 -> 雷达像素。纯定点：内核禁用浮点，这里一个 float 都不能有。
 * wx/wz/sx/sz 都是 Q16.16 米，span 换成定点后做除法得到 [-1,1] 的标量。
 */
static void hx_radar_map(fx_t wx, fx_t wz, fx_t sx, fx_t sz, int *px, int *py)
{
	s32 span_m = g_cfg.world_span > 1 ? g_cfg.world_span : 200;
	s32 R = g_cfg.radar_r > 8 ? g_cfg.radar_r : 180;
	fx_t span = FX_INT(span_m);
	fx_t rx, rz;

	if (span <= 0)
		span = FX_INT(200);

	rx = FX_DIV(wx - sx, span);
	rz = FX_DIV(wz - sz, span);

	*px = g_cfg.radar_cx + (int)(((s64)rx * R) >> FX_SHIFT);
	*py = g_cfg.radar_cy + (int)(((s64)rz * R) >> FX_SHIFT);
}

/* 中点圆：只依赖整数，kprobe 上下文安全 */
static void hx_circle(struct hx_fb_cache *c, int cx, int cy, int r, u32 color)
{
	int x = r, y = 0, err = 1 - r;

	if (r <= 0 || r > 2048)
		return;
	while (x >= y) {
		hx_put_px(c, cx + x, cy + y, color);
		hx_put_px(c, cx + y, cy + x, color);
		hx_put_px(c, cx - y, cy + x, color);
		hx_put_px(c, cx - x, cy + y, color);
		hx_put_px(c, cx - x, cy - y, color);
		hx_put_px(c, cx - y, cy - x, color);
		hx_put_px(c, cx + y, cy - x, color);
		hx_put_px(c, cx + x, cy - y, color);
		y++;
		if (err < 0)
			err += 2 * y + 1;
		else {
			x--;
			err += 2 * (y - x) + 1;
		}
	}
}

/*
 * 世界坐标 -> 屏幕（透视）。矩阵投影是 4x4 浮点乘加，内核态禁用浮点，
 * 所以这里直接关掉，只用雷达模式（雷达不需要相机矩阵）。
 * 要做透视的话，得把矩阵按 Q16.16 读入再做整数投影。
 */
static bool hx_w2s(int pid, fx_t wx, fx_t wz, int *ox, int *oy)
{
	(void)pid; (void)wx; (void)wz; (void)ox; (void)oy;
	return false;
}

static void hx_draw_locked(void)
{
	struct hx_fb_cache *c = &g_fbc;
	struct hx_ent *e = g_last;
	int n = g_last_n, i;
	int pid = g_stat.last_pid;

	if (!c->valid || n <= 0)
		return;

	/* 雷达底盘 */
	if (g_cfg.ly_radar.on) {
		int R = g_cfg.radar_r ? g_cfg.radar_r : 180;

		hx_circle(c, g_cfg.radar_cx, g_cfg.radar_cy, R,
			  g_cfg.ly_radar.color & 0x60FFFFFFu);
		hx_line(c, g_cfg.radar_cx - 6, g_cfg.radar_cy,
			g_cfg.radar_cx + 6, g_cfg.radar_cy,
			g_cfg.ly_radar.color, 1);
		hx_line(c, g_cfg.radar_cx, g_cfg.radar_cy - 6,
			g_cfg.radar_cx, g_cfg.radar_cy + 6,
			g_cfg.ly_radar.color, 1);
	}

	for (i = 1; i < n; i++) {          /* 0 是自己 */
		int px = 0, py = 0;
		bool have = false;
		u32 hpcol;

		/* 阵营过滤：team 有效且与自己相同则跳过 */
		if ((g_cfg.flags & HX_FLAG_CAMP_FILTER) && e[i].team >= 0 &&
		    e[i].team == e[0].team)
			continue;

		if (g_cfg.ly_radar.on) {
			hx_radar_map(e[i].x, e[i].z, e[0].x, e[0].z, &px, &py);
			have = true;
			hx_dot(c, px, py, 3, g_cfg.ly_radar.color);
		}

		if (g_cfg.w2s_addr) {
			int sx, sy;

			if (hx_w2s(pid, e[i].x, e[i].z, &sx, &sy)) {
				int bw = 46, bh = 92;   /* 固定框尺寸，避免距离抖动被统计 */

				if (g_cfg.ly_box.on)
					hx_rect(c, sx - bw / 2, sy - bh / 2, bw, bh,
						g_cfg.ly_box.color, g_cfg.ly_box.thick);
				if (g_cfg.ly_line.on)
					hx_line(c, g_cfg.hud_w / 2, g_cfg.hud_h - 8,
						sx, sy, g_cfg.ly_line.color, 1);

				if (g_cfg.ly_hp.on && e[i].hp > 0) {
					int hh = 60;
					int fill = hh * min(e[i].hp, 20000) / 20000;

					hpcol = (e[i].hp < 400 || fill < hh / 3)
						? 0xFFFF3030u : 0xFF30FF30u;
					hx_rect(c, sx - bw / 2 - 8, sy - hh / 2, 4, hh,
						0xFF000000u, 1);
					hx_rect(c, sx - bw / 2 - 8, sy - hh / 2 + (hh - fill),
						4, fill, hpcol, 1);
				}
			}
		}
	}
}

static void hx_map_work_fn(struct work_struct *w)
{
	struct drm_framebuffer *fb;
	struct iosys_map map;
	int r;

	spin_lock(&g_lock);
	fb = g_fbc.fb;
	spin_unlock(&g_lock);
	if (!fb || !fb->obj[0])
		return;

	if (g_fbc.vaddr) {
		dma_buf_vunmap(fb->obj[0]->dma_buf, &map);
		g_fbc.vaddr = NULL;
	}
	{
		struct dma_buf *db = fb->obj[0]->dma_buf;

		if (!db)
			return;
		r = dma_buf_vmap(db, &map);
		if (r)
			return;
		g_fbc.vaddr = map.vaddr;
		g_fbc.pitch = fb->pitches[0];
		g_fbc.cpp = fb->format ? fb->format->cpp[0] : 4;
		g_fbc.w = fb->width;
		g_fbc.h = fb->height;
		g_fbc.format = fb->format ? fb->format->format : 0;
		g_fbc.valid = true;
	}
}

/*
 * 入口钩子：drm_atomic_commit(struct drm_device *dev, struct drm_atomic_state *state, bool nonblock)
 * 这里只做纯内存操作，不睡眠。
 */
static int hx_commit_pre(struct kprobe *kp, struct pt_regs *regs)
{
	struct drm_device *dev = (void *)regs->regs[0];
	struct drm_atomic_state *state = (void *)regs->regs[1];
	struct drm_framebuffer *fb = NULL;

	if (!g_ddev && dev)
		g_ddev = dev;
	if (!state)
		return 0;

	/*
	 * 这个内核的 struct drm_atomic_state 里连 num_plane / num_crtcs 都没有
	 * （oplus 的 DRM 分支动过结构），所以不能靠 state 的数组长度。
	 *
	 * 改成从「device 的 crtc 链表 → crtc->primary → plane->state->fb」取。
	 * 在 drm_atomic_commit 入口，plane->state 指向的是当前已生效的那一帧，
	 * 对 ESP 叠画来说够用（双缓冲下画哪一帧都会送到屏幕）。
	 * 全程只读指针，不睡眠，kprobe 上下文安全。
	 */
	{
		struct drm_crtc *crtc;

		drm_for_each_crtc(crtc, dev) {
			struct drm_plane *pp = crtc->primary;

			if (!pp || !pp->state)
				continue;
			if (pp->state->fb) {
				fb = pp->state->fb;
				break;
			}
		}
	}
	if (!fb)
		return 0;

	if (g_fbc.fb != fb) {
		spin_lock(&g_lock);
		g_fbc.fb = fb;
		g_fbc.valid = false;
		spin_unlock(&g_lock);
		schedule_work(&g_map_work);
		return 0;
	}

	if (!g_fbc.valid || !g_fbc.vaddr)
		return 0;

	/*
	 * 绝不能在这里调 dma_buf_begin_cpu_access()/end_cpu_access()：
	 * 它们会睡眠，而 kprobe pre_handler 处于原子上下文 —— 一调用就是
	 * "scheduling while atomic" + panic，这正是上一次 bootloop 的元凶。
	 * 这里只做纯内存写，cache 一致性交给平台的 IO-coherent；
	 * 最坏情况是某帧没画上，不会崩。
	 */
	if (!(g_cfg.flags & HX_FLAG_DRM_DRAW))
		return 0;

	spin_lock(&g_lock);
	hx_draw_locked();
	spin_unlock(&g_lock);

	return 0;
}

/* 采集线程把快照丢进来（避免在 kprobe 里做内存读取） */
void hx_drm_frame(const struct hx_ent *e, int n, int self_team)
{
	(void)self_team;
	spin_lock(&g_lock);
	if (n > HX_MAX_ENT)
		n = HX_MAX_ENT;
	memcpy(g_last, e, sizeof(struct hx_ent) * n);
	g_last_n = n;
	spin_unlock(&g_lock);
}

int hx_drm_init(void)
{
	spin_lock_init(&g_lock);
	INIT_WORK(&g_map_work, hx_map_work_fn);
	g_kp_commit.symbol_name = "drm_atomic_commit";
	if (register_kprobe(&g_kp_commit)) {
		hx_warn("drm: hook failed, radar via userspace fallback\n");
		return -ENOENT;
	}
	return 0;
}

void hx_drm_exit(void)
{
	cancel_work_sync(&g_map_work);
	unregister_kprobe(&g_kp_commit);
	if (g_fbc.vaddr && g_fbc.fb && g_fbc.fb->obj[0]) {
		struct iosys_map map = { .vaddr = g_fbc.vaddr };

		dma_buf_vunmap(g_fbc.fb->obj[0]->dma_buf, &map);
	}
	g_fbc.valid = false;
}
