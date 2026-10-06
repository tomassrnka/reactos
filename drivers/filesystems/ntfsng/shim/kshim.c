// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * kshim.c - kernel-mode implementations of the Linux kernel APIs that the
 * fs/ntfs read path calls, ported from the user-mode spike shim.  Memory is
 * nonpaged pool, locks are NT dispatcher objects and spin locks, block I/O
 * is IRP-based, all through ngos.h.  APIs only reached from write, mmap,
 * ioctl or VFS paths the NT glue never drives are generated into
 * kshim_stubs.c and stop the system with their name if called.
 */
#include <kshim.h>

struct task_struct kshim_task;
struct user_namespace init_user_ns;
struct mnt_idmap nop_mnt_idmap;
const char hex_asc[] = "0123456789abcdef";
int kshim_verbose;
static uintptr_t kshim_a64_lock;
static uintptr_t kshim_pc_lock;
static DEFINE_MUTEX(kshim_inode_lock);

/* ------------------------------------------------------------- printk */
int printk(const char *fmt, ...)
{
	va_list ap;
	int lvl = 4;
	char *buf;

	if (fmt[0] == '<' && fmt[1] >= '0' && fmt[1] <= '7' && fmt[2] == '>') {
		lvl = fmt[1] - '0';
		fmt += 3;
	}
	if (lvl > 4 && !kshim_verbose)
		return 0;
	buf = ngos_alloc(512);
	if (!buf)
		return 0;
	memcpy(buf, "ntfsng: ", 8);
	va_start(ap, fmt);
	vsnprintf(buf + 8, 512 - 8, fmt, ap);
	va_end(ap);
	ngos_print(buf);
	ngos_free(buf);
	return 0;
}

void kshim_bug(const char *file, int line)
{
	ngos_bugcheck("BUG", file, line);
}

void kshim_unimpl(const char *name)
{
	ngos_bugcheck(name, "unimplemented Linux API", 0);
}

/* ------------------------------------------------------------- memory */
/*
 * A 16-byte header (on x86) keeps the size for krealloc and the allocating
 * call site, so live allocations can be attributed when hunting leaks.
 */
struct khdr { size_t n; size_t magic; void *caller; size_t pad; };
#define KMAGIC ((size_t)0x4e474b4dUL)
#define KSITES 1024
static struct { void *caller; long live; long long bytes; } ksites[KSITES];
static uintptr_t ksites_lock;
long kshim_alloc_live;
long long kshim_alloc_bytes;
static void ksite_account(void *caller, long d, long long bytes)
{
	unsigned int h = ((uintptr_t)caller >> 2) % KSITES;
	unsigned char irql = ngos_spin_lock(&ksites_lock);
	for (unsigned int i = 0; i < KSITES; i++, h = (h + 1) % KSITES) {
		if (ksites[h].caller == caller || !ksites[h].caller) {
			ksites[h].caller = caller;
			ksites[h].live += d;
			ksites[h].bytes += bytes;
			break;
		}
	}
	kshim_alloc_live += d;
	kshim_alloc_bytes += bytes;
	ngos_spin_unlock(&ksites_lock, irql);
}
void kshim_dump_allocs(void)
{
	int top[12], nt = 0;
	for (int k = 0; k < 12; k++) {
		int best = -1;
		for (int i = 0; i < KSITES; i++) {
			int seen = 0;
			for (int j = 0; j < nt; j++) if (top[j] == i) seen = 1;
			if (!seen && ksites[i].caller && (best < 0 || ksites[i].bytes > ksites[best].bytes)) best = i;
		}
		if (best < 0 || ksites[best].live <= 0) break;
		top[nt++] = best;
	}
	printk("allocs: %ld live, %lld bytes; top call sites:\n", kshim_alloc_live, kshim_alloc_bytes);
	for (int k = 0; k < nt; k++)
		printk("  site %p: %ld live, %lld bytes\n", ksites[top[k]].caller, ksites[top[k]].live, ksites[top[k]].bytes);
}
static void *kmalloc_site(size_t n, gfp_t g, void *caller)
{
	struct khdr *h = ngos_alloc(sizeof(*h) + (n ? n : 1));
	if (!h)
		return NULL;
	h->n = n; h->magic = KMAGIC; h->caller = caller;
	ksite_account(caller, 1, n);
	if (g & __GFP_ZERO)
		memset(h + 1, 0, n);
	return h + 1;
}
void *kmalloc(size_t n, gfp_t g) { return kmalloc_site(n, g, __builtin_return_address(0)); }
void *kzalloc(size_t n, gfp_t g) { return kmalloc_site(n, g | __GFP_ZERO, __builtin_return_address(0)); }
void *kcalloc(size_t n, size_t s, gfp_t g)
{
	size_t t;
	if (__builtin_mul_overflow(n, s, &t))
		return NULL;
	return kmalloc_site(t, g | __GFP_ZERO, __builtin_return_address(0));
}
void *kmalloc_array(size_t n, size_t s, gfp_t g)
{
	size_t t;
	if (__builtin_mul_overflow(n, s, &t))
		return NULL;
	return kmalloc_site(t, g, __builtin_return_address(0));
}
void kfree(const void *p)
{
	struct khdr *h;
	if (!p || IS_ERR(p))
		return;
	h = (struct khdr *)p - 1;
	if (h->magic != KMAGIC)
		ngos_bugcheck("kfree of a foreign pointer", __FILE__, __LINE__);
	h->magic = 0;
	ksite_account(h->caller, -1, -(long long)h->n);
	ngos_free(h);
}
void *krealloc(const void *p, size_t n, gfp_t g)
{
	void *q;
	size_t old;
	if (!p)
		return kmalloc(n, g);
	old = ((struct khdr *)p - 1)->n;
	q = kmalloc(n, g);
	if (!q)
		return NULL;
	memcpy(q, p, old < n ? old : n);
	kfree(p);
	return q;
}
char *kstrdup(const char *s, gfp_t g) { return s ? kmemdup(s, strlen(s) + 1, g) : NULL; }
char *kstrndup(const char *s, size_t n, gfp_t g)
{
	size_t l = 0;
	char *p;
	if (!s)
		return NULL;
	while (l < n && s[l]) l++;
	p = kmalloc(l + 1, g);
	if (p) { memcpy(p, s, l); p[l] = 0; }
	return p;
}
void *kmemdup(const void *s, size_t n, gfp_t g) { void *p = kmalloc_site(n, g, __builtin_return_address(0)); if (p) memcpy(p, s, n); return p; }
unsigned long totalram_pages(void) { return 1UL << 18; }
struct kmem_cache *kmem_cache_create(const char *name, unsigned int size,
		unsigned int align, unsigned long flags, void (*ctor)(void *))
{
	struct kmem_cache *c = kzalloc(sizeof(*c), GFP_KERNEL);
	(void)align; (void)flags;
	if (!c)
		return NULL;
	c->size = size; c->name = name; c->ctor = ctor;
	return c;
}
void kmem_cache_destroy(struct kmem_cache *c) { kfree(c); }
void *kmem_cache_alloc(struct kmem_cache *c, gfp_t g)
{
	void *p = kmalloc_site(c->size, g | __GFP_ZERO, __builtin_return_address(0));
	if (p && c->ctor) c->ctor(p);
	return p;
}
void *kmem_cache_zalloc(struct kmem_cache *c, gfp_t g) { return kmem_cache_alloc(c, g); }
void kmem_cache_free(struct kmem_cache *c, void *p) { (void)c; kfree(p); }
unsigned long __get_free_page(gfp_t g) { return (uintptr_t)kmalloc(PAGE_SIZE, g); }
void free_page(unsigned long p) { kfree((void *)(uintptr_t)p); }

/* ------------------------------------------------------------- atomics */
s64 kshim_atomic64_add(atomic64_t *v, s64 d)
{
	unsigned char irql = ngos_spin_lock(&kshim_a64_lock);
	s64 r = (v->counter += d);
	ngos_spin_unlock(&kshim_a64_lock, irql);
	return r;
}
void kshim_atomic64_set(atomic64_t *v, s64 i)
{
	unsigned char irql = ngos_spin_lock(&kshim_a64_lock);
	v->counter = i;
	ngos_spin_unlock(&kshim_a64_lock, irql);
}

/* --------------------------------------------------------------- locks */
static void mutex_ready(struct mutex *m)
{
	int st = __atomic_load_n(&m->state, __ATOMIC_ACQUIRE);
	if (st == 2)
		return;
	st = 0;
	if (__atomic_compare_exchange_n(&m->state, &st, 1, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
		ngos_ev_init(&m->ev, 1, 1);
		__atomic_store_n(&m->state, 2, __ATOMIC_RELEASE);
		return;
	}
	while (__atomic_load_n(&m->state, __ATOMIC_ACQUIRE) != 2)
		ngos_yield();
}
void mutex_init(struct mutex *m) { ngos_ev_init(&m->ev, 1, 1); m->state = 2; }
void mutex_lock(struct mutex *m) { mutex_ready(m); ngos_ev_wait(&m->ev); }
int mutex_trylock(struct mutex *m) { mutex_ready(m); return ngos_ev_trywait(&m->ev); }
void mutex_unlock(struct mutex *m) { ngos_ev_set(&m->ev); }
int mutex_is_locked(struct mutex *m) { mutex_ready(m); return !ngos_ev_state(&m->ev); }

void read_lock(rwlock_t *l)
{
	for (;;) {
		long c = __atomic_load_n(&l->cnt, __ATOMIC_ACQUIRE);
		if (c >= 0 && __atomic_compare_exchange_n(&l->cnt, &c, c + 1, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
			return;
		ngos_yield();
	}
}
void read_unlock(rwlock_t *l) { __atomic_sub_fetch(&l->cnt, 1, __ATOMIC_SEQ_CST); }
void write_lock(rwlock_t *l)
{
	for (;;) {
		long c = 0;
		if (__atomic_compare_exchange_n(&l->cnt, &c, -1, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
			return;
		ngos_yield();
	}
}
void write_unlock(rwlock_t *l) { __atomic_store_n(&l->cnt, 0, __ATOMIC_SEQ_CST); }

static void rwsem_ready(struct rw_semaphore *s)
{
	int st = __atomic_load_n(&s->state, __ATOMIC_ACQUIRE);
	if (st == 2)
		return;
	st = 0;
	if (__atomic_compare_exchange_n(&s->state, &st, 1, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
		s->sl = 0; s->readers = 0; s->writer = 0;
		ngos_ev_init(&s->wake, 0, 0);
		__atomic_store_n(&s->state, 2, __ATOMIC_RELEASE);
		return;
	}
	while (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) != 2)
		ngos_yield();
}
void init_rwsem(struct rw_semaphore *s)
{
	s->sl = 0; s->readers = 0; s->writer = 0;
	ngos_ev_init(&s->wake, 0, 0);
	s->state = 2;
}
static int rwsem_try(struct rw_semaphore *s, int write, int block)
{
	rwsem_ready(s);
	for (;;) {
		unsigned char irql = ngos_spin_lock(&s->sl);
		if (write ? (!s->writer && !s->readers) : !s->writer) {
			if (write) s->writer = 1; else s->readers++;
			ngos_spin_unlock(&s->sl, irql);
			return 1;
		}
		if (!block) {
			ngos_spin_unlock(&s->sl, irql);
			return 0;
		}
		ngos_ev_clear(&s->wake);
		ngos_spin_unlock(&s->sl, irql);
		ngos_ev_wait(&s->wake);
	}
}
void down_read(struct rw_semaphore *s) { rwsem_try(s, 0, 1); }
void down_write(struct rw_semaphore *s) { rwsem_try(s, 1, 1); }
int down_read_trylock(struct rw_semaphore *s) { return rwsem_try(s, 0, 0); }
int down_write_trylock(struct rw_semaphore *s) { return rwsem_try(s, 1, 0); }
void up_read(struct rw_semaphore *s)
{
	unsigned char irql = ngos_spin_lock(&s->sl);
	if (--s->readers == 0)
		ngos_ev_set(&s->wake);
	ngos_spin_unlock(&s->sl, irql);
}
void up_write(struct rw_semaphore *s)
{
	unsigned char irql = ngos_spin_lock(&s->sl);
	s->writer = 0;
	ngos_ev_set(&s->wake);
	ngos_spin_unlock(&s->sl, irql);
}
void downgrade_write(struct rw_semaphore *s)
{
	unsigned char irql = ngos_spin_lock(&s->sl);
	s->writer = 0; s->readers++;
	ngos_ev_set(&s->wake);
	ngos_spin_unlock(&s->sl, irql);
}
int rwsem_is_locked(struct rw_semaphore *s) { rwsem_ready(s); return s->writer || s->readers; }

/* ------------------------------------------------------------- bitmaps */
unsigned long find_next_bit(const unsigned long *a, unsigned long size, unsigned long off)
{
	for (; off < size; off++) if (test_bit(off, a)) return off;
	return size;
}
unsigned long find_next_zero_bit(const unsigned long *a, unsigned long size, unsigned long off)
{
	for (; off < size; off++) if (!test_bit(off, a)) return off;
	return size;
}
unsigned int bitmap_weight(const unsigned long *a, unsigned int bits)
{
	unsigned int w = 0;
	for (unsigned int i = 0; i < bits; i++) w += test_bit(i, a);
	return w;
}

/* --------------------------------------------- rbtree (unbalanced BST) */
#define RBP(n) ((struct rb_node *)((n)->__rb_parent_color))
void rb_insert_color(struct rb_node *n, struct rb_root *r) { (void)n; (void)r; }
struct rb_node *rb_first(const struct rb_root *r)
{
	struct rb_node *n = r->rb_node;
	if (!n) return NULL;
	while (n->rb_left) n = n->rb_left;
	return n;
}
struct rb_node *rb_next(const struct rb_node *n)
{
	struct rb_node *p;
	if (n->rb_right) { n = n->rb_right; while (n->rb_left) n = n->rb_left; return (struct rb_node *)n; }
	while ((p = RBP(n)) && n == p->rb_right) n = p;
	return p;
}
static void rb_replace_child(struct rb_node *old, struct rb_node *new_, struct rb_root *r)
{
	struct rb_node *p = RBP(old);
	if (!p) r->rb_node = new_;
	else if (p->rb_left == old) p->rb_left = new_;
	else p->rb_right = new_;
	if (new_) new_->__rb_parent_color = (uintptr_t)p;
}
void rb_erase(struct rb_node *n, struct rb_root *r)
{
	if (!n->rb_left) rb_replace_child(n, n->rb_right, r);
	else if (!n->rb_right) rb_replace_child(n, n->rb_left, r);
	else {
		struct rb_node *s = n->rb_right;
		while (s->rb_left) s = s->rb_left;
		if (RBP(s) != n) {
			rb_replace_child(s, s->rb_right, r);
			s->rb_right = n->rb_right;
			s->rb_right->__rb_parent_color = (uintptr_t)s;
		}
		rb_replace_child(n, s, r);
		s->rb_left = n->rb_left;
		s->rb_left->__rb_parent_color = (uintptr_t)s;
	}
}

/* ---------------------------------------------------------------- time */
void ktime_get_coarse_real_ts64(struct timespec64 *ts) { long ns; ngos_time(&ts->tv_sec, &ns); ts->tv_nsec = ns; }
void ktime_get_real_ts64(struct timespec64 *ts) { ktime_get_coarse_real_ts64(ts); }
s64 ktime_get_real_seconds(void) { struct timespec64 t; ktime_get_coarse_real_ts64(&t); return t.tv_sec; }
struct timespec64 current_time(struct inode *i) { struct timespec64 t; (void)i; ktime_get_coarse_real_ts64(&t); return t; }
struct timespec64 inode_set_ctime_current(struct inode *i) { i->i_ctime = current_time(i); return i->i_ctime; }
void simple_inode_init_ts(struct inode *i) { i->i_atime = i->i_mtime = i->i_ctime = current_time(i); }

/* ------------------------------------------------------------ workqueue */
struct workqueue_struct kshim_wq;
struct workqueue_struct *alloc_workqueue(const char *fmt, unsigned int flags, int max, ...)
{ (void)fmt; (void)flags; (void)max; return &kshim_wq; }
void destroy_workqueue(struct workqueue_struct *wq) { (void)wq; }
void flush_workqueue(struct workqueue_struct *wq) { (void)wq; }
/* Work items run synchronously in the caller (the read-only paths queue only the free-cluster precount). */
bool queue_work(struct workqueue_struct *wq, struct work_struct *w) { (void)wq; w->func(w); return true; }
bool schedule_work(struct work_struct *w) { w->func(w); return true; }
bool cancel_work_sync(struct work_struct *w) { (void)w; return false; }
void flush_work(struct work_struct *w) { (void)w; }

/* ------------------------------------------------------------------ nls */
static int utf8_dec(const u8 *s, int len, u32 *out)
{
	if (len < 1) return -1;
	if (s[0] < 0x80) { *out = s[0]; return 1; }
	if ((s[0] & 0xe0) == 0xc0 && len >= 2) { *out = ((s[0] & 0x1f) << 6) | (s[1] & 0x3f); return 2; }
	if ((s[0] & 0xf0) == 0xe0 && len >= 3) { *out = ((s[0] & 0x0f) << 12) | ((s[1] & 0x3f) << 6) | (s[2] & 0x3f); return 3; }
	if ((s[0] & 0xf8) == 0xf0 && len >= 4) { *out = ((s[0] & 0x07) << 18) | ((s[1] & 0x3f) << 12) | ((s[2] & 0x3f) << 6) | (s[3] & 0x3f); return 4; }
	return -1;
}
static int utf8_enc(u32 c, u8 *o, int max)
{
	if (c < 0x80 && max >= 1) { o[0] = c; return 1; }
	if (c < 0x800 && max >= 2) { o[0] = 0xc0 | (c >> 6); o[1] = 0x80 | (c & 0x3f); return 2; }
	if (c < 0x10000 && max >= 3) { o[0] = 0xe0 | (c >> 12); o[1] = 0x80 | ((c >> 6) & 0x3f); o[2] = 0x80 | (c & 0x3f); return 3; }
	if (max >= 4) { o[0] = 0xf0 | (c >> 18); o[1] = 0x80 | ((c >> 12) & 0x3f); o[2] = 0x80 | ((c >> 6) & 0x3f); o[3] = 0x80 | (c & 0x3f); return 4; }
	return -1;
}
int utf8s_to_utf16s(const u8 *s, int len, enum utf16_endian e, wchar_t_ *pwcs, int maxlen)
{
	int o = 0; (void)e;
	while (len > 0 && *s) {
		u32 c; int n = utf8_dec(s, len, &c);
		if (n < 0) return -EINVAL;
		s += n; len -= n;
		if (c >= 0x10000) {
			if (o + 2 > maxlen) break;
			c -= 0x10000; pwcs[o++] = 0xd800 | (c >> 10); pwcs[o++] = 0xdc00 | (c & 0x3ff);
		} else {
			if (o + 1 > maxlen) break;
			pwcs[o++] = c;
		}
	}
	return o;
}
int utf16s_to_utf8s(const wchar_t_ *pwcs, int len, enum utf16_endian e, u8 *s, int maxlen)
{
	int o = 0; (void)e;
	while (len > 0 && *pwcs) {
		u32 c = *pwcs++; len--;
		if (c >= 0xd800 && c < 0xdc00 && len > 0 && *pwcs >= 0xdc00 && *pwcs < 0xe000) {
			c = 0x10000 + ((c - 0xd800) << 10) + (*pwcs++ - 0xdc00); len--;
		}
		int n = utf8_enc(c, s + o, maxlen - o);
		if (n < 0) break;
		o += n;
	}
	return o;
}
static int utf8_uni2char(wchar_t_ uni, unsigned char *out, int boundlen) { int n = utf8_enc(uni, out, boundlen); return n < 0 ? -ENAMETOOLONG : n; }
static int utf8_char2uni(const unsigned char *raw, int boundlen, wchar_t_ *uni)
{ u32 c; int n = utf8_dec(raw, boundlen, &c); if (n < 0 || c > 0xffff) return -EINVAL; *uni = c; return n; }
static struct nls_table kshim_utf8 = { "utf8", utf8_uni2char, utf8_char2uni, NULL };
struct nls_table *load_nls(const char *charset) { return strcmp(charset, "utf8") ? NULL : &kshim_utf8; }
struct nls_table *load_nls_default(void) { return &kshim_utf8; }
void unload_nls(struct nls_table *t) { (void)t; }

/* ------------------------------------------------------------ fs types */
struct file_system_type *kshim_fs_type;
int register_filesystem(struct file_system_type *t) { kshim_fs_type = t; return 0; }
int unregister_filesystem(struct file_system_type *t) { (void)t; kshim_fs_type = NULL; return 0; }
struct ctl_table_header kshim_ctl;
struct ctl_table_header *register_sysctl(const char *p, const struct ctl_table *t) { (void)p; (void)t; return &kshim_ctl; }
void unregister_sysctl_table(struct ctl_table_header *h) { (void)h; }

/* ------------------------------------------------------- block device */
unsigned long kshim_counter_reads, kshim_counter_refused_writes;
int blk_status_to_errno(blk_status_t s) { return s ? -EIO : 0; }
int kshim_dev_rw(struct block_device *b, int write, u64 off, void *buf, size_t len)
{
	if (write) {
		kshim_counter_refused_writes++;
		printk(KERN_ERR "refusing device write at %llu len %zu (read-only driver)\n",
				(unsigned long long)off, len);
		return -EROFS;
	}
	kshim_counter_reads++;
	if (off >= b->size) {
		memset(buf, 0, len);
		return 0;
	}
	if (off + len > b->size) {
		size_t in = (size_t)(b->size - off);
		memset((char *)buf + in, 0, len - in);
		len = in;
	}
	return ngos_dev_read(b->osdev, off, buf, (unsigned int)len) ? -EIO : 0;
}
int bdev_rw_virt(struct block_device *b, sector_t s, void *data, size_t len, blk_opf_t op)
{
	return kshim_dev_rw(b, (op & REQ_OP_MASK) == REQ_OP_WRITE, s << SECTOR_SHIFT, data, len);
}
struct bio *bio_alloc(struct block_device *b, unsigned short nr, blk_opf_t op, gfp_t g)
{
	struct bio *bio = kzalloc(sizeof(*bio), g);
	if (!bio)
		return NULL;
	bio->bi_bdev = b; bio->bi_opf = op;
	bio->bi_max_vecs = nr ? nr : 1;
	bio->bi_io_vec = kcalloc(bio->bi_max_vecs, sizeof(struct bio_vec), g);
	if (!bio->bi_io_vec) { kfree(bio); return NULL; }
	return bio;
}
void bio_put(struct bio *b) { kfree(b->bi_io_vec); kfree(b); }
int bio_add_page(struct bio *b, struct page *p, unsigned int len, unsigned int off)
{
	/* Like the block layer, merge a range contiguous with the last segment of the same page. */
	if (b->bi_vcnt) {
		struct bio_vec *v = &b->bi_io_vec[b->bi_vcnt - 1];
		if (v->bv_page == p && v->bv_offset + v->bv_len == off) {
			v->bv_len += len; b->bi_iter.bi_size += len; return len;
		}
	}
	if (b->bi_vcnt >= b->bi_max_vecs) return 0;
	b->bi_io_vec[b->bi_vcnt++] = (struct bio_vec){ p, len, off };
	b->bi_iter.bi_size += len;
	return len;
}
bool bio_add_folio(struct bio *b, struct folio *f, size_t len, size_t off) { return bio_add_page(b, &f->page, len, off) > 0; }
void bio_add_folio_nofail(struct bio *b, struct folio *f, size_t len, size_t off) { BUG_ON(!bio_add_folio(b, f, len, off)); }
void bio_chain(struct bio *b, struct bio *parent) { b->kshim_chain = parent; }
static int kshim_bio_do(struct bio *b)
{
	u64 off = b->bi_iter.bi_sector << SECTOR_SHIFT;
	int write = (b->bi_opf & REQ_OP_MASK) == REQ_OP_WRITE, err = 0;
	for (unsigned i = 0; i < b->bi_vcnt; i++) {
		struct bio_vec *v = &b->bi_io_vec[i];
		int e = kshim_dev_rw(b->bi_bdev, write, off, (char *)v->bv_page->data + v->bv_offset, v->bv_len);
		if (e) err = e;
		off += v->bv_len;
	}
	b->bi_status = err ? BLK_STS_IOERR : BLK_STS_OK;
	return err;
}
void submit_bio(struct bio *b)
{
	kshim_bio_do(b);
	if (b->bi_end_io) b->bi_end_io(b);
}
int submit_bio_wait(struct bio *b) { return kshim_bio_do(b); }

/* ----------------------------------------------------------- page cache */
/*
 * Private metadata/data page cache: a per-mapping hash of 4 KiB folios in
 * nonpaged pool.  One shim-wide spin lock guards the hash chains; folio
 * contents are guarded by PG_locked as in Linux.  A mapping keeps at most
 * KSHIM_PC_MAX pages: beyond that, unreferenced clean folios are dropped.
 */
#define KSHIM_PC_MAX 1024
unsigned long kshim_pc_pages;
static struct folio *pc_find_locked(struct address_space *m, pgoff_t idx)
{
	for (struct folio *f = m->pc[idx % KSHIM_PC_BUCKETS]; f; f = f->hnext)
		if (f->index == idx) return f;
	return NULL;
}
static void pc_free(struct folio *f)
{
	kfree(f->data);
	kfree(f);
	__atomic_sub_fetch(&kshim_pc_pages, 1, __ATOMIC_SEQ_CST);
}
/* Unlinks unreferenced clean unlocked folios of @m; returns them chained through hnext. */
static struct folio *pc_shrink_locked(struct address_space *m, pgoff_t keep)
{
	struct folio *out = NULL;
	for (int b = 0; b < KSHIM_PC_BUCKETS; b++) {
		struct folio **pp = &m->pc[b];
		while (*pp) {
			struct folio *f = *pp;
			if (f->index != keep && f->refcount == 1 && !(f->flags & ((1UL << PG_locked) | (1UL << PG_dirty) | (1UL << PG_writeback)))) {
				*pp = f->hnext;
				m->nrpages--;
				f->hnext = out;
				out = f;
			} else {
				pp = &f->hnext;
			}
		}
	}
	return out;
}
void kshim_folio_lock(struct folio *f)
{
	while (test_and_set_bit(PG_locked, &f->flags))
		ngos_yield();
}
void folio_get(struct folio *f) { __atomic_add_fetch(&f->refcount, 1, __ATOMIC_SEQ_CST); }
void folio_put(struct folio *f)
{
	int r = __atomic_sub_fetch(&f->refcount, 1, __ATOMIC_SEQ_CST);
	BUG_ON(r < 0);
	/* A folio outside any mapping (alloc_page) is freed with its last reference. */
	if (r == 0 && !f->mapping)
		pc_free(f);
}
struct page *alloc_page(gfp_t g)
{
	struct folio *f = kzalloc(sizeof(struct folio), g);
	if (!f)
		return NULL;
	f->data = kzalloc(PAGE_SIZE, g);
	if (!f->data) { kfree(f); return NULL; }
	f->refcount = 1;
	__atomic_add_fetch(&kshim_pc_pages, 1, __ATOMIC_SEQ_CST);
	return &f->page;
}
void __free_page(struct page *p)
{
	struct folio *f = page_folio(p);
	if (f && !f->mapping)
		pc_free(f);
}
struct folio *__filemap_get_folio(struct address_space *m, pgoff_t idx, fgf_t fgp, gfp_t g)
{
	struct folio *f, *nf = NULL, *drop = NULL;
	unsigned char irql;
	(void)g;
	irql = ngos_spin_lock(&kshim_pc_lock);
	f = pc_find_locked(m, idx);
	if (f)
		f->refcount++;
	ngos_spin_unlock(&kshim_pc_lock, irql);
	if (!f) {
		if (!(fgp & FGP_CREAT))
			return ERR_PTR(-ENOENT);
		nf = kzalloc(sizeof(*nf), GFP_NOFS);
		if (!nf)
			return ERR_PTR(-ENOMEM);
		nf->data = kzalloc(PAGE_SIZE, GFP_NOFS);
		if (!nf->data) { kfree(nf); return ERR_PTR(-ENOMEM); }
		nf->mapping = m; nf->index = idx; nf->refcount = 2;  /* the cache's own reference + ours */
		irql = ngos_spin_lock(&kshim_pc_lock);
		f = pc_find_locked(m, idx);
		if (f) {
			f->refcount++;
		} else {
			if (m->nrpages >= KSHIM_PC_MAX)
				drop = pc_shrink_locked(m, idx);
			nf->hnext = m->pc[idx % KSHIM_PC_BUCKETS];
			m->pc[idx % KSHIM_PC_BUCKETS] = nf;
			m->nrpages++;
			f = nf;
			nf = NULL;
			__atomic_add_fetch(&kshim_pc_pages, 1, __ATOMIC_SEQ_CST);
		}
		ngos_spin_unlock(&kshim_pc_lock, irql);
		if (nf) { kfree(nf->data); kfree(nf); }
		while (drop) { struct folio *n = drop->hnext; pc_free(drop); drop = n; }
	}
	if (fgp & FGP_LOCK) folio_lock(f);
	return f;
}
struct folio *filemap_lock_folio(struct address_space *m, pgoff_t idx) { return __filemap_get_folio(m, idx, FGP_LOCK, 0); }
struct folio *filemap_grab_folio(struct address_space *m, pgoff_t idx) { return __filemap_get_folio(m, idx, FGP_LOCK | FGP_CREAT, 0); }
struct folio *filemap_get_folio(struct address_space *m, pgoff_t idx) { return __filemap_get_folio(m, idx, 0, 0); }
struct page *grab_cache_page_nowait(struct address_space *m, pgoff_t idx)
{
	struct folio *f = __filemap_get_folio(m, idx, FGP_LOCK | FGP_CREAT | FGP_NOWAIT, 0);
	return IS_ERR(f) ? NULL : &f->page;
}
struct folio *read_mapping_folio(struct address_space *m, pgoff_t idx, struct file *file)
{
	struct folio *f = __filemap_get_folio(m, idx, FGP_CREAT, 0);
	int err;
	if (IS_ERR(f)) return f;
	if (folio_test_uptodate(f)) return f;
	folio_lock(f);
	if (folio_test_uptodate(f)) { folio_unlock(f); return f; }
	err = m->a_ops->read_folio(file, f);
	if (folio_test_locked(f)) {
		printk(KERN_ERR "read_folio left folio %lu locked\n", (unsigned long)idx);
		folio_unlock(f);
	}
	if (err || !folio_test_uptodate(f)) {
		folio_put(f);
		return ERR_PTR(err ? err : -EIO);
	}
	return f;
}
struct page *read_mapping_page(struct address_space *m, pgoff_t idx, struct file *file)
{
	struct folio *f = read_mapping_folio(m, idx, file);
	return IS_ERR(f) ? (struct page *)f : &f->page;
}
bool folio_mark_dirty(struct folio *f)
{
	printk(KERN_ERR "folio_mark_dirty on a read-only mount (index %lu)\n", (unsigned long)f->index);
	return !test_and_set_bit(PG_dirty, &f->flags);
}
bool filemap_dirty_folio(struct address_space *m, struct folio *f) { (void)m; return folio_mark_dirty(f); }
void __mark_inode_dirty(struct inode *i, int flags) { i->i_state |= flags; }
int filemap_write_and_wait(struct address_space *m) { (void)m; return 0; }
int filemap_write_and_wait_range(struct address_space *m, loff_t s, loff_t e) { (void)m; (void)s; (void)e; return 0; }
void file_ra_state_init(struct file_ra_state *ra, struct address_space *m) { (void)m; ra->ra_pages = 32; }
void page_cache_sync_readahead(struct address_space *m, void *ra, struct file *f, pgoff_t i, unsigned long n)
{ (void)m; (void)ra; (void)f; (void)i; (void)n; }
static void pc_drop_all(struct address_space *m)
{
	struct folio *list = NULL;
	unsigned char irql = ngos_spin_lock(&kshim_pc_lock);
	for (int b = 0; b < KSHIM_PC_BUCKETS; b++) {
		struct folio *f = m->pc[b];
		while (f) {
			struct folio *n = f->hnext;
			if (f->refcount > 1)
				printk(KERN_ERR "dropping referenced folio %lu (ref %d)\n", (unsigned long)f->index, f->refcount);
			f->hnext = list; list = f;
			f = n;
		}
		m->pc[b] = NULL;
	}
	m->nrpages = 0;
	ngos_spin_unlock(&kshim_pc_lock, irql);
	while (list) { struct folio *n = list->hnext; pc_free(list); list = n; }
}
/* Drops the clean unreferenced folios of a mapping (used after data reads to bound memory). */
void kshim_mapping_shrink(struct address_space *m)
{
	struct folio *drop;
	unsigned char irql = ngos_spin_lock(&kshim_pc_lock);
	drop = pc_shrink_locked(m, (pgoff_t)-1);
	ngos_spin_unlock(&kshim_pc_lock, irql);
	while (drop) { struct folio *n = drop->hnext; pc_free(drop); drop = n; }
}
void truncate_inode_pages(struct address_space *m, loff_t l) { (void)l; pc_drop_all(m); }
void truncate_inode_pages_final(struct address_space *m) { pc_drop_all(m); }

/* ------------------------------------------------------------------ iomap */
int iomap_iter_continue(const struct iomap_iter *iter, struct iomap *iomap,
		struct iomap *srcmap, int ret)
{
	const bool stale = iomap->flags & IOMAP_F_STALE;
	const ssize_t_ advanced = iter->pos - iter->iter_start_pos;
	if (ret < 0 && !advanced) return ret;
	if (iter->status < 0) ret = iter->status;
	else if (iter->len == 0 || (!advanced && !stale)) ret = 0;
	else ret = 1;
	if (ret <= 0) return ret;
	memset(iomap, 0, sizeof(*iomap));
	memset(srcmap, 0, sizeof(*srcmap));
	return ret;
}
int iomap_iter(struct iomap_iter *iter, const struct iomap_ops *ops)
{
	int ret;
	if (ops->iomap_next)
		ret = ops->iomap_next(iter, &iter->iomap, &iter->srcmap);
	else
		ret = iomap_iter_next(iter, &iter->iomap, &iter->srcmap, ops->iomap_begin, ops->iomap_end);
	iter->status = 0;
	if (ret > 0)
		iter->iter_start_pos = iter->pos;
	return ret;
}
int iomap_iter_advance(struct iomap_iter *iter, u64 count)
{
	if (count > iter->len) return -EIO;
	iter->pos += count; iter->len -= count;
	return 0;
}
static const struct iomap *iomap_iter_srcmap(const struct iomap_iter *i)
{ return i->srcmap.type != IOMAP_HOLE ? &i->srcmap : &i->iomap; }
static inline sector_t iomap_sector(const struct iomap *iomap, loff_t pos)
{ return (iomap->addr + pos - iomap->offset) >> SECTOR_SHIFT; }
/* Per-folio pending byte count stands in for struct iomap_folio_state. */
struct kshim_rstate { long pending; int err; };
void iomap_finish_folio_read(struct folio *folio, size_t off, size_t len, int error)
{
	struct kshim_rstate *st = folio->private;
	(void)off;
	if (!st) { if (!error) folio_mark_uptodate(folio); folio_unlock(folio); return; }
	if (error) st->err = error;
	st->pending -= len;
}
int iomap_bio_read_folio_range(const struct iomap_iter *iter, struct iomap_read_folio_ctx *ctx, size_t plen)
{
	struct folio *folio = ctx->cur_folio;
	struct bio *bio = ctx->read_ctx;
	if (!bio || bio_end_sector(bio) != iomap_sector(&iter->iomap, iter->pos) ||
	    !bio_add_folio(bio, folio, plen, offset_in_folio(folio, iter->pos))) {
		if (bio) ctx->ops->submit_read(iter, ctx);
		bio = bio_alloc(iter->iomap.bdev, 16, REQ_OP_READ, GFP_NOFS);
		if (!bio) return -ENOMEM;
		bio->bi_iter.bi_sector = iomap_sector(&iter->iomap, iter->pos);
		bio_add_folio_nofail(bio, folio, plen, offset_in_folio(folio, iter->pos));
		ctx->read_ctx = bio;
		ctx->read_ctx_file_offset = iter->pos;
	}
	return 0;
}
void iomap_bio_submit_read_endio(const struct iomap_iter *iter, struct iomap_read_folio_ctx *ctx, bio_end_io_t end_io)
{
	struct bio *bio = ctx->read_ctx;
	(void)iter;
	if (!bio) return;
	bio->bi_end_io = end_io;
	submit_bio(bio);
	ctx->read_ctx = NULL;
}
/* Follows fs/iomap/buffered-io.c iomap_read_folio_iter() for order-0 folios. */
static int kshim_read_folio_iter(struct iomap_iter *iter, struct iomap_read_folio_ctx *ctx, struct kshim_rstate *st)
{
	const struct iomap *iomap = &iter->iomap;
	struct folio *folio = ctx->cur_folio;
	loff_t isize = i_size_read(iter->inode);
	u64 length = iomap_length(iter);
	unsigned bsize = 1U << iter->inode->i_blkbits;

	if (iomap->type == IOMAP_INLINE) {
		const struct iomap *src = iomap_iter_srcmap(iter);
		size_t size = isize - src->offset;
		size_t offset = offset_in_folio(folio, src->offset);
		if (!src->inline_data || size > src->length) return -EIO;
		folio_fill_tail(folio, offset, src->inline_data, size);
		return iomap_iter_advance(iter, length);
	}
	length = min_t(u64, length, PAGE_SIZE - offset_in_folio(folio, iter->pos));
	while (length) {
		loff_t pos = iter->pos;
		size_t poff = offset_in_folio(folio, pos);
		u64 plen = length;
		int r;
		if (pos < isize && pos + plen > isize)
			plen = min_t(u64, plen, round_up(isize, bsize) - pos);
		if (iomap_iter_srcmap(iter)->type != IOMAP_MAPPED ||
		    (iomap_iter_srcmap(iter)->flags & IOMAP_F_NEW) || pos >= isize) {
			folio_zero_range(folio, poff, plen);
		} else {
			int ret;
			st->pending += plen;
			ret = ctx->ops->read_folio_range(iter, ctx, plen);
			if (ret) return ret;
		}
		length -= plen;
		r = iomap_iter_advance(iter, plen);
		if (r) return r;
	}
	return 0;
}
void iomap_read_folio(const struct iomap_ops *ops, struct iomap_read_folio_ctx *ctx, void *private)
{
	struct folio *folio = ctx->cur_folio;
	struct kshim_rstate st = { 0, 0 };
	struct iomap_iter iter = {
		.inode = folio->mapping->host, .pos = folio_pos(folio),
		.len = folio_size(folio), .private = private,
	};
	int ret;
	iter.iter_start_pos = iter.pos;
	folio->private = &st;
	while ((ret = iomap_iter(&iter, ops)) > 0)
		iter.status = kshim_read_folio_iter(&iter, ctx, &st);
	if (ctx->ops->submit_read) ctx->ops->submit_read(&iter, ctx);
	folio->private = NULL;
	if (ret < 0 || st.err || st.pending) {
		printk(KERN_ERR "iomap_read_folio idx %lu failed ret=%d err=%d pending=%ld\n",
				(unsigned long)folio->index, ret, st.err, st.pending);
		folio_clear_uptodate(folio);
	} else {
		folio_mark_uptodate(folio);
	}
	folio_unlock(folio);
}

/* ------------------------------------------------------------------ inode */
/*
 * Inode cache: a per-superblock list under one shim mutex.  Unlike Linux,
 * an inode is evicted and freed as soon as its last reference goes: the NT
 * glue keeps its own FCB references, and Linux may evict any unused inode
 * at any time anyway.
 */
unsigned long kshim_inodes_live;
void inode_init_once(struct inode *i) { memset(i, 0, sizeof(*i)); }
static void kshim_inode_init(struct super_block *sb, struct inode *i)
{
	i->i_sb = sb;
	i->i_blkbits = sb->s_blocksize_bits;
	i->i_count.counter = 1;
	i->i_nlink = 1;
	i->i_mapping = &i->i_data;
	i->i_data.host = i;
	i->i_data.gfp_mask = GFP_NOFS;
	init_rwsem(&i->i_rwsem);
	init_rwsem(&i->i_data.invalidate_lock);
	INIT_LIST_HEAD(&i->i_data.private_list);
	i->kshim_next = sb->kshim_inodes;
	sb->kshim_inodes = i;
	kshim_inodes_live++;
}
static void kshim_inode_unlist(struct inode *i)
{
	struct inode **pp = &i->i_sb->kshim_inodes;
	while (*pp && *pp != i)
		pp = &(*pp)->kshim_next;
	if (*pp) {
		*pp = i->kshim_next;
		kshim_inodes_live--;
	}
}
struct inode *new_inode(struct super_block *sb)
{
	struct inode *i = sb->s_op->alloc_inode(sb);
	if (!i) return NULL;
	mutex_lock(&kshim_inode_lock);
	kshim_inode_init(sb, i);
	mutex_unlock(&kshim_inode_lock);
	return i;
}
void insert_inode_hash(struct inode *i) { i->i_hash.pprev = (void *)1; i->kshim_test_data = (void *)(uintptr_t)i->i_ino; }
void remove_inode_hash(struct inode *i) { i->i_hash.pprev = NULL; }
static struct inode *kshim_find(struct super_block *sb, unsigned long hashval,
		int (*test)(struct inode *, void *), void *data)
{
	for (struct inode *i = sb->kshim_inodes; i; i = i->kshim_next)
		if (i->i_hash.pprev && (uintptr_t)i->kshim_test_data == hashval &&
		    !(i->i_state & I_FREEING) && (!test || test(i, data)))
			return i;
	return NULL;
}
static struct inode *kshim_find_get(struct super_block *sb, unsigned long hashval,
		int (*test)(struct inode *, void *), void *data)
{
	struct inode *i;
	for (;;) {
		mutex_lock(&kshim_inode_lock);
		i = kshim_find(sb, hashval, test, data);
		if (i && (i->i_state & I_NEW) && i->i_count.counter > 0) {
			/* Another thread is still filling it in: wait like wait_on_inode(). */
			mutex_unlock(&kshim_inode_lock);
			ngos_yield();
			continue;
		}
		if (i) atomic_inc(&i->i_count);
		mutex_unlock(&kshim_inode_lock);
		return i;
	}
}
struct inode *ilookup5(struct super_block *sb, unsigned long hashval,
		int (*test)(struct inode *, void *), void *data)
{
	return kshim_find_get(sb, hashval, test, data);
}
struct inode *ilookup5_nowait(struct super_block *sb, unsigned long hashval,
		int (*test)(struct inode *, void *), void *data, bool *isnew)
{
	struct inode *i;
	mutex_lock(&kshim_inode_lock);
	i = kshim_find(sb, hashval, test, data);
	if (i) atomic_inc(&i->i_count);
	mutex_unlock(&kshim_inode_lock);
	if (isnew) *isnew = i ? (i->i_state & I_NEW) != 0 : false;
	return i;
}
struct inode *iget5_locked(struct super_block *sb, unsigned long hashval,
		int (*test)(struct inode *, void *), int (*set)(struct inode *, void *), void *data)
{
	struct inode *i = kshim_find_get(sb, hashval, test, data), *n;
	if (i) return i;
	n = sb->s_op->alloc_inode(sb);
	if (!n) return NULL;
	mutex_lock(&kshim_inode_lock);
	i = kshim_find(sb, hashval, test, data);
	if (i && !(i->i_state & I_NEW)) {
		atomic_inc(&i->i_count);
		mutex_unlock(&kshim_inode_lock);
		if (sb->s_op->free_inode) sb->s_op->free_inode(n);
		else if (sb->s_op->destroy_inode) sb->s_op->destroy_inode(n);
		return i;
	}
	kshim_inode_init(sb, n);
	if (set && set(n, data)) {
		n->i_state |= I_FREEING;
		kshim_inode_unlist(n);
		mutex_unlock(&kshim_inode_lock);
		return NULL;
	}
	n->kshim_test_data = (void *)(uintptr_t)hashval;
	n->i_state |= I_NEW;
	n->i_hash.pprev = (void *)1;
	mutex_unlock(&kshim_inode_lock);
	return n;
}
/* The mft inode created by fill_super is hashed via insert_inode_hash with i_ino. */
struct inode *find_inode_nowait(struct super_block *sb, unsigned long hashval,
		int (*match)(struct inode *, u64, void *), void *data)
{
	struct inode *ret = NULL;
	mutex_lock(&kshim_inode_lock);
	for (struct inode *i = sb->kshim_inodes; i; i = i->kshim_next) {
		int r;
		if (!i->i_hash.pprev) continue;
		r = match(i, hashval, data);
		if (r) { ret = r > 0 ? i : NULL; break; }
	}
	mutex_unlock(&kshim_inode_lock);
	return ret;
}
void unlock_new_inode(struct inode *i) { i->i_state &= ~I_NEW; }
void discard_new_inode(struct inode *i) { i->i_state &= ~I_NEW; i->i_state |= I_FREEING; iput(i); }
void iget_failed(struct inode *i) { discard_new_inode(i); }
struct inode *igrab(struct inode *i) { atomic_inc(&i->i_count); return i; }
void ihold(struct inode *i) { atomic_inc(&i->i_count); }
static void kshim_evict(struct inode *i)
{
	struct super_block *sb = i->i_sb;
	i->i_state |= I_FREEING;
	mutex_lock(&kshim_inode_lock);
	i->i_hash.pprev = NULL;
	mutex_unlock(&kshim_inode_lock);
	if (sb->s_op->evict_inode) sb->s_op->evict_inode(i);
	else truncate_inode_pages_final(i->i_mapping);
	mutex_lock(&kshim_inode_lock);
	kshim_inode_unlist(i);
	mutex_unlock(&kshim_inode_lock);
	if (sb->s_op->destroy_inode) sb->s_op->destroy_inode(i);
	if (sb->s_op->free_inode) sb->s_op->free_inode(i);
}
void iput(struct inode *i)
{
	if (!i || IS_ERR(i))
		return;
	if (!atomic_dec_and_test(&i->i_count))
		return;
	if (i->i_sb->s_op->drop_inode)
		(void)i->i_sb->s_op->drop_inode(i);
	kshim_evict(i);
}
void clear_inode(struct inode *i) { i->i_state |= I_CLEAR; }
int generic_delete_inode(struct inode *i) { (void)i; return 1; }
int inode_generic_drop(struct inode *i) { return !i->i_nlink; }
void set_nlink(struct inode *i, unsigned int n) { i->i_nlink = n; }
void inc_nlink(struct inode *i) { i->i_nlink++; }
void drop_nlink(struct inode *i) { i->i_nlink--; }
void clear_nlink(struct inode *i) { i->i_nlink = 0; }
void init_special_inode(struct inode *i, umode_t m, dev_t d) { i->i_mode = m; i->i_rdev = d; }
void inode_init_owner(struct mnt_idmap *idmap, struct inode *i, const struct inode *dir, umode_t mode)
{ (void)idmap; (void)dir; i->i_mode = mode; }

/* ---------------------------------------------------------------- dentry */
struct dentry *d_make_root(struct inode *i)
{
	struct dentry *d;
	if (!i) return NULL;
	d = kzalloc(sizeof(*d), GFP_KERNEL);
	if (!d) { iput(i); return NULL; }
	d->d_inode = i; d->d_parent = d; d->d_sb = i->i_sb;
	d->d_name.name = (const unsigned char *)"/"; d->d_name.len = 1;
	return d;
}
struct dentry *d_splice_alias(struct inode *i, struct dentry *d)
{
	if (IS_ERR(i)) return (struct dentry *)i;
	d->d_inode = i;
	return NULL;
}
struct dentry *d_add_ci(struct dentry *d, struct inode *i, struct qstr *n) { (void)n; return d_splice_alias(i, d); }
void d_add(struct dentry *d, struct inode *i) { d->d_inode = i; }
void dput(struct dentry *d) { (void)d; }
bool dir_emit_dots(struct file *f, struct dir_context *ctx)
{
	if (ctx->pos == 0) {
		if (!ctx->actor(ctx, ".", 1, ctx->pos, f->f_inode->i_ino, DT_DIR)) return false;
		ctx->pos = 1;
	}
	if (ctx->pos == 1) {
		struct dentry *p = f->f_path.dentry ? f->f_path.dentry->d_parent : NULL;
		u64 ino = p && p->d_inode ? p->d_inode->i_ino : f->f_inode->i_ino;
		if (!ctx->actor(ctx, "..", 2, ctx->pos, ino, DT_DIR)) return false;
		ctx->pos = 2;
	}
	return true;
}
unsigned int full_name_hash(const void *salt, const char *n, unsigned int len)
{
	unsigned int h = 0; (void)salt;
	while (len--) h = h * 31 + (unsigned char)*n++;
	return h;
}

/* --------------------------------------------------------------- super */
int sb_set_blocksize(struct super_block *sb, int size)
{
	if (size < 512 || size > (int)PAGE_SIZE || (size & (size - 1))) return 0;
	sb->s_blocksize = size;
	sb->s_blocksize_bits = ilog2(size);
	return size;
}
int sb_min_blocksize(struct super_block *sb, int size)
{
	int min = bdev_logical_block_size(sb->s_bdev);
	return sb_set_blocksize(sb, size < min ? min : size);
}
/* Set by ngcore around get_tree under its mount mutex. */
struct block_device *kshim_mount_bdev;
int get_tree_bdev(struct fs_context *fc, int (*fill_super)(struct super_block *, struct fs_context *))
{
	struct super_block *sb = kzalloc(sizeof(*sb), GFP_KERNEL);
	int err;
	if (!sb)
		return -ENOMEM;
	sb->s_bdev = kshim_mount_bdev;
	sb->s_type = fc->fs_type;
	sb->s_flags = fc->sb_flags;
	sb->s_fs_info = fc->s_fs_info;
	sb->s_user_ns = &init_user_ns;
	fc->s_fs_info = NULL;
	snprintf(sb->s_id, sizeof(sb->s_id), "ntfsng%p", (void *)sb);
	sb_set_blocksize(sb, 512);
	err = fill_super(sb, fc);
	if (err) { kfree(sb); return err; }
	fc->root = sb->s_root;
	fc->kshim_sb = sb;
	return 0;
}
static int kshim_vmsg(const char *pfx, const char *fmt, va_list ap)
{
	char *buf = ngos_alloc(384);
	if (!buf) return 0;
	vsnprintf(buf, 384, fmt, ap);
	printk("%s%s\n", pfx, buf);
	ngos_free(buf);
	return 0;
}
int invalf(struct fs_context *fc, const char *fmt, ...) { va_list ap; (void)fc; va_start(ap, fmt); kshim_vmsg("invalid: ", fmt, ap); va_end(ap); return -EINVAL; }
int errorf(struct fs_context *fc, const char *fmt, ...) { va_list ap; (void)fc; va_start(ap, fmt); kshim_vmsg("error: ", fmt, ap); va_end(ap); return -EINVAL; }
int warnf(struct fs_context *fc, const char *fmt, ...) { va_list ap; (void)fc; va_start(ap, fmt); kshim_vmsg("warning: ", fmt, ap); va_end(ap); return 0; }
int sync_blockdev(struct block_device *b) { (void)b; return 0; }
int sync_filesystem(struct super_block *sb) { (void)sb; return 0; }
void seq_printf(struct seq_file *m, const char *fmt, ...) { (void)m; (void)fmt; }
void seq_puts(struct seq_file *m, const char *s) { (void)m; (void)s; }

/* Block device page cache (Linux def_blk_aops equivalent): used by compress.c and bdev-io.c. */
static int kshim_blkdev_read_folio(struct file *f, struct folio *folio)
{
	struct block_device *b = folio->mapping->host->i_private;
	int err = kshim_dev_rw(b, 0, (u64)folio->index << PAGE_SHIFT, folio->data, PAGE_SIZE);
	(void)f;
	if (!err) folio_mark_uptodate(folio);
	folio_unlock(folio);
	return err;
}
static const struct address_space_operations kshim_blkdev_aops = { .read_folio = kshim_blkdev_read_folio };
struct block_device *kshim_bdev_open(void *osdev, u64 size, unsigned int sector_size)
{
	struct block_device *b = kzalloc(sizeof(*b), GFP_KERNEL);
	struct inode *bi = kzalloc(sizeof(*bi), GFP_KERNEL);
	if (!b || !bi) { kfree(b); kfree(bi); return NULL; }
	b->osdev = osdev;
	b->size = size;
	b->logical_block_size = sector_size;
	bi->i_private = b;
	bi->i_size = size;
	bi->i_blkbits = PAGE_SHIFT;
	bi->i_mapping = &bi->i_data;
	bi->i_data.host = bi;
	bi->i_data.a_ops = &kshim_blkdev_aops;
	init_rwsem(&bi->i_rwsem);
	init_rwsem(&bi->i_data.invalidate_lock);
	b->bd_inode = bi;
	b->bd_mapping = bi->i_mapping;
	return b;
}
void kshim_bdev_close(struct block_device *b)
{
	if (!b) return;
	truncate_inode_pages_final(b->bd_mapping);
	kfree(b->bd_inode);
	kfree(b);
}

/* ------------------------------------------------------------- misc libc */
void sort(void *base, size_t num, size_t size, int (*cmp)(const void *, const void *), void (*swp)(void *, void *, int))
{
	/* Insertion sort: fs/ntfs sorts short arrays only. */
	char *b = base;
	for (size_t i = 1; i < num; i++)
		for (size_t j = i; j > 0 && cmp(b + (j - 1) * size, b + j * size) > 0; j--) {
			if (swp) swp(b + (j - 1) * size, b + j * size, (int)size);
			else for (size_t k = 0; k < size; k++) { char t = b[(j - 1) * size + k]; b[(j - 1) * size + k] = b[j * size + k]; b[j * size + k] = t; }
		}
}
int hex_to_bin(unsigned char ch)
{
	if (ch >= '0' && ch <= '9') return ch - '0';
	ch |= 0x20;
	if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
	return -1;
}
