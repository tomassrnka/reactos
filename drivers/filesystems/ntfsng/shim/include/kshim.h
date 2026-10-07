/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * kshim.h - kernel-mode (ReactOS/NT port of the spike's user-mode) stand-in for the Linux kernel APIs used by
 * fs/ntfs.  Single-threaded probe: locks are no-ops, the page cache is a
 * per-mapping hash of order-0 folios, block I/O is pread/pwrite on an image.
 * Nothing in here implements NTFS logic.
 */
#ifndef KSHIM_H
#define KSHIM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
#include <stdarg.h>
#include "ngos.h"
#include "kshim_errno.h"

typedef long ssize_t;
typedef long long kshim_time_t;
#define time_t kshim_time_t
typedef unsigned int dev_t;
typedef unsigned int mode_t;
typedef int pid_t;
typedef long long off_t_k;
typedef unsigned long long blkcnt_t;
int snprintf(char *buf, size_t n, const char *fmt, ...);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int scnprintf(char *buf, size_t n, const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);

/* ---------------------------------------------------------------- types */
typedef uint8_t u8;   typedef int8_t s8;
typedef uint16_t u16; typedef int16_t s16;
typedef uint32_t u32; typedef int32_t s32;
typedef unsigned long long u64; typedef long long s64;
typedef u8 __u8; typedef u16 __u16; typedef u32 __u32; typedef u64 __u64;
typedef s8 __s8; typedef s16 __s16; typedef s32 __s32; typedef s64 __s64;
typedef u16 __le16; typedef u32 __le32; typedef u64 __le64;
typedef u16 __be16; typedef u32 __be32; typedef u64 __be64;
typedef u16 __sle16; typedef u32 __sle32; typedef u64 __sle64;
typedef long long loff_t_;
#define loff_t long long
typedef unsigned long pgoff_t;
typedef u64 sector_t;

typedef unsigned int gfp_t;
typedef unsigned short umode_t;
typedef unsigned int fmode_t;
typedef long ssize_t_;
typedef u32 blk_opf_t;
typedef u8 blk_status_t;
typedef u32 errseq_t;
typedef struct { u32 val; } kuid_t;
typedef struct { u32 val; } kgid_t;
typedef u32 uid_t_;
typedef u32 vfsuid_t;
typedef u32 vfsgid_t;
typedef u64 phys_addr_t;
typedef int __bitwise_dummy;

#define __bitwise
#define __force
#define __user
#define __iomem
#define __rcu
#define __must_check
#undef __always_inline
#define __always_inline inline __attribute__((always_inline))
#define noinline __attribute__((noinline))
#define __init
#define __exit
#define __initdata
#define __read_mostly
#define __maybe_unused __attribute__((unused))
#define __always_unused __attribute__((unused))
#define __packed __attribute__((packed))
#define __aligned(x) __attribute__((aligned(x)))
#define __printf(a, b) __attribute__((format(printf, a, b)))
#define __cold
#define __counted_by(x)
#define __nonstring
#define __fallthrough
#define fallthrough __attribute__((fallthrough))
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define READ_ONCE(x) (*(volatile __typeof__(x) *)&(x))
#define WRITE_ONCE(x, v) (*(volatile __typeof__(x) *)&(x) = (v))
#define barrier() __asm__ __volatile__("" ::: "memory")
#define smp_mb() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define smp_rmb() smp_mb()
#define smp_wmb() smp_mb()
#define smp_mb__after_atomic() smp_mb()
#define smp_mb__before_atomic() smp_mb()
#define EXPORT_SYMBOL(x)
#define EXPORT_SYMBOL_GPL(x)
#define KBUILD_MODNAME "ntfs"
#define static_assert(...) _Static_assert(__VA_ARGS__, #__VA_ARGS__)
#define BUILD_BUG_ON(c) _Static_assert(!(c), #c)
#define __free(f) __attribute__((cleanup(__free_##f)))
void kfree(const void *p);
static inline void __free_kfree(void *p) { kfree(*(void **)p); }
static inline void __free_kvfree(void *p) { kfree(*(void **)p); }
#define unsafe_memcpy(d, s, n, j) memcpy(d, s, n)
#define __must_be_array(a) 0
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define sizeof_field(T, m) sizeof(((T *)0)->m)
#define container_of(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))
#define struct_size(p, member, n) (sizeof(*(p)) + sizeof(*(p)->member) * (n))
#define flex_array_size(p, member, n) (sizeof(*(p)->member) * (n))
#define typecheck(t, x) 1

/* --------------------------------------------------------------- limits */
#define U8_MAX 0xff
#define U16_MAX 0xffff
#define U32_MAX 0xffffffffU
#define S32_MAX INT_MAX
#define S64_MAX ((s64)0x7fffffffffffffffLL)
#define S64_MIN (-S64_MAX - 1)
#define U64_MAX (~0ULL)
#define LLONG_MAX_ LLONG_MAX
#define MAX_LFS_FILESIZE ((loff_t)LLONG_MAX)
#define PAGE_SHIFT 12
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#define PAGE_MASK (~(PAGE_SIZE - 1))
#define SECTOR_SHIFT 9
#define SECTOR_SIZE (1 << SECTOR_SHIFT)
#define BITS_PER_LONG (__SIZEOF_LONG__ * 8)
#define BITS_PER_BYTE 8
#define BIT(n) (1UL << (n))
#define BIT_ULL(n) (1ULL << (n))
#define GENMASK(h, l) (((~0UL) << (l)) & (~0UL >> (BITS_PER_LONG - 1 - (h))))

/* ---------------------------------------------------------------- math */
#define min(a, b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a < _b ? _a : _b; })
#define max(a, b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a > _b ? _a : _b; })
#define min_t(t, a, b) ({ t _a = (a); t _b = (b); _a < _b ? _a : _b; })
#define max_t(t, a, b) ({ t _a = (a); t _b = (b); _a > _b ? _a : _b; })
#define min3(a, b, c) min(min(a, b), c)
#define clamp(v, lo, hi) min(max(v, lo), hi)
#define swap(a, b) do { __typeof__(a) _t = (a); (a) = (b); (b) = _t; } while (0)
#define abs(x) ({ __typeof__(x) _x = (x); _x < 0 ? -_x : _x; })
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define DIV_ROUND_UP_ULL(n, d) ((u64)(((u64)(n) + (d) - 1) / (d)))
#define round_up(x, y) ((((x) - 1) | ((__typeof__(x))((y) - 1))) + 1)
#define round_down(x, y) ((x) & ~((__typeof__(x))((y) - 1)))
#define ALIGN(x, a) (((x) + ((__typeof__(x))(a) - 1)) & ~((__typeof__(x))(a) - 1))
#define ALIGN_DOWN(x, a) round_down(x, a)
#define IS_ALIGNED(x, a) (((x) & ((__typeof__(x))(a) - 1)) == 0)
#define is_power_of_2(n) ((n) != 0 && (((n) & ((n) - 1)) == 0))
#define cmp_int(l, r) (((l) > (r)) - ((l) < (r)))
#define do_div(n, base) ({ u32 _rem = (u64)(n) % (base); (n) = (u64)(n) / (base); _rem; })
static inline u64 div_u64(u64 a, u32 b) { return a / b; }
static inline s64 div_s64(s64 a, s32 b) { return a / b; }
static inline u64 div64_u64(u64 a, u64 b) { return a / b; }
static inline s64 div_s64_rem(s64 a, s32 b, s32 *r) { *r = a % b; return a / b; }
static inline u64 div_u64_rem(u64 a, u32 b, u32 *r) { *r = a % b; return a / b; }
#define check_add_overflow(a, b, d) __builtin_add_overflow(a, b, d)
#define check_mul_overflow(a, b, d) __builtin_mul_overflow(a, b, d)
#define check_sub_overflow(a, b, d) __builtin_sub_overflow(a, b, d)
#define overflows_type(x, T) ((__typeof__(x))(__typeof__(T))(x) != (x))
static inline int ilog2_u64(u64 v) { return 63 - __builtin_clzll(v); }
#define ilog2(v) ilog2_u64((u64)(v))
static inline int fls(unsigned int x) { return x ? 32 - __builtin_clz(x) : 0; }
static inline int fls64(u64 x) { return x ? 64 - __builtin_clzll(x) : 0; }
static inline unsigned long __ffs(unsigned long x) { return __builtin_ctzl(x); }
static inline unsigned long ffz(unsigned long x) { return __builtin_ctzl(~x); }
#define hweight8(x) __builtin_popcount((u8)(x))
#define hweight32(x) __builtin_popcount((u32)(x))
#define hweight64(x) __builtin_popcountll((u64)(x))
#define hweight_long(x) __builtin_popcountl(x)

/* ----------------------------------------------------------- endianness */
#define cpu_to_le16(x) ((__le16)(u16)(x))
#define cpu_to_le32(x) ((__le32)(u32)(x))
#define cpu_to_le64(x) ((__le64)(u64)(x))
#define le16_to_cpu(x) ((u16)(x))
#define le32_to_cpu(x) ((u32)(x))
#define le64_to_cpu(x) ((u64)(x))
#define cpu_to_be32(x) __builtin_bswap32(x)
#define be32_to_cpu(x) __builtin_bswap32(x)
#define cpu_to_be16(x) __builtin_bswap16(x)
#define be16_to_cpu(x) __builtin_bswap16(x)
#define cpu_to_be64(x) __builtin_bswap64(x)
#define be64_to_cpu(x) __builtin_bswap64(x)
#define le16_to_cpup(p) (*(const u16 *)(p))
#define le32_to_cpup(p) (*(const u32 *)(p))
#define le64_to_cpup(p) (*(const u64 *)(p))
#define cpu_to_le16p(p) (*(const u16 *)(p))
#define le16_add_cpu(p, v) (*(p) += (v))
#define le32_add_cpu(p, v) (*(p) += (v))
#define le64_add_cpu(p, v) (*(p) += (v))
#define get_unaligned_le16(p) ({ u16 _v; memcpy(&_v, (p), 2); _v; })
#define get_unaligned_le32(p) ({ u32 _v; memcpy(&_v, (p), 4); _v; })
#define get_unaligned_le64(p) ({ u64 _v; memcpy(&_v, (p), 8); _v; })
#define put_unaligned_le16(v, p) do { u16 _v = (v); memcpy((p), &_v, 2); } while (0)
#define put_unaligned_le32(v, p) do { u32 _v = (v); memcpy((p), &_v, 4); } while (0)
#define put_unaligned_le64(v, p) do { u64 _v = (v); memcpy((p), &_v, 8); } while (0)
#define get_unaligned(p) ({ __typeof__(*(p)) _v; memcpy(&_v, (p), sizeof(_v)); _v; })
#define put_unaligned(v, p) do { __typeof__(*(p)) _v = (v); memcpy((p), &_v, sizeof(_v)); } while (0)

/* ----------------------------------------------------------------- errors */
#define ERESTARTSYS 512
#define ENOIOCTLCMD 515
#define ENOTSUPP 524
#define EFSCORRUPTED EUCLEAN
#define MAX_ERRNO 4095
#define IS_ERR_VALUE(x) unlikely((uintptr_t)(void *)(x) >= (uintptr_t)-MAX_ERRNO)
static inline void *ERR_PTR(long e) { return (void *)(intptr_t)e; }
static inline long PTR_ERR(const void *p) { return (long)(intptr_t)p; }
static inline bool IS_ERR(const void *p) { return IS_ERR_VALUE((uintptr_t)p); }
static inline bool IS_ERR_OR_NULL(const void *p) { return !p || IS_ERR(p); }
static inline void *ERR_CAST(const void *p) { return (void *)p; }
static inline int PTR_ERR_OR_ZERO(const void *p) { return IS_ERR(p) ? PTR_ERR(p) : 0; }

/* ----------------------------------------------------------------- printk */
#define KERN_EMERG "<0>"
#define KERN_ALERT "<1>"
#define KERN_CRIT "<2>"
#define KERN_ERR "<3>"
#define KERN_WARNING "<4>"
#define KERN_NOTICE "<5>"
#define KERN_INFO "<6>"
#define KERN_DEBUG "<7>"
#define KERN_CONT ""
extern int kshim_verbose;
int printk(const char *fmt, ...) __printf(1, 2);
#ifndef pr_fmt
#define pr_fmt(fmt) fmt
#endif
#define pr_err(fmt, ...) printk(KERN_ERR pr_fmt(fmt), ##__VA_ARGS__)
#define pr_warn(fmt, ...) printk(KERN_WARNING pr_fmt(fmt), ##__VA_ARGS__)
#define pr_info(fmt, ...) printk(KERN_INFO pr_fmt(fmt), ##__VA_ARGS__)
#define pr_notice(fmt, ...) printk(KERN_NOTICE pr_fmt(fmt), ##__VA_ARGS__)
#define pr_crit(fmt, ...) printk(KERN_CRIT pr_fmt(fmt), ##__VA_ARGS__)
#define pr_debug(fmt, ...) do { if (0) printk(fmt, ##__VA_ARGS__); } while (0)
#define pr_cont(fmt, ...) printk(fmt, ##__VA_ARGS__)
#define pr_err_ratelimited pr_err
#define pr_warn_ratelimited pr_warn
#define pr_info_ratelimited pr_info
#define pr_warn_once pr_warn
#define printk_ratelimit() 1
#define no_printk(fmt, ...) ({ if (0) printk(fmt, ##__VA_ARGS__); 0; })
void kshim_bug(const char *file, int line) __attribute__((noreturn));
#define BUG() kshim_bug(__FILE__, __LINE__)
#define BUG_ON(c) do { if (unlikely(c)) BUG(); } while (0)
#define WARN_ON(c) ({ int _c = !!(c); if (unlikely(_c)) printk("WARN_ON(%s) at %s:%d\n", #c, __FILE__, __LINE__); unlikely(_c); })
#define WARN_ON_ONCE(c) WARN_ON(c)
#define WARN(c, fmt, ...) ({ int _c = !!(c); if (unlikely(_c)) printk("WARN: " fmt, ##__VA_ARGS__); unlikely(_c); })
#define WARN_ONCE WARN
#define VM_BUG_ON(c) BUG_ON(c)
#define might_sleep() do { } while (0)
#define cond_resched() do { } while (0)
#define schedule() ngos_yield()
#define panic(fmt, ...) do { printk(fmt, ##__VA_ARGS__); kshim_bug(__FILE__, __LINE__); } while (0)
#define dump_stack() do { } while (0)

/* -------------------------------------------------------------- string */
static inline ssize_t_ strscpy(char *d, const char *s, size_t n)
{
	size_t l = strlen(s);
	if (!n) return -E2BIG;
	if (l >= n) { memcpy(d, s, n - 1); d[n - 1] = 0; return -E2BIG; }
	memcpy(d, s, l + 1); return l;
}
#define strscpy_pad strscpy
char *kstrdup(const char *s, gfp_t g);
char *kstrndup(const char *s, size_t n, gfp_t g);
void *kmemdup(const void *s, size_t n, gfp_t g);
static inline char *strreplace(char *s, char old, char new_)
{ for (; *s; s++) if (*s == old) *s = new_; return s; }
static inline const char *str_plural(size_t n) { return n == 1 ? "" : "s"; }
static inline const char *str_yes_no(bool v) { return v ? "yes" : "no"; }
static inline const char *str_on_off(bool v) { return v ? "on" : "off"; }
static inline bool mem_is_zero(const void *s, size_t n)
{ const u8 *p = s; while (n--) if (*p++) return false; return true; }
#define memzero_explicit(p, n) memset(p, 0, n)
extern const char hex_asc[];
#define hex_asc_lo(x) hex_asc[((x) & 0x0f)]
#define hex_asc_hi(x) hex_asc[((x) & 0xf0) >> 4]
int hex_to_bin(unsigned char ch);

/* ------------------------------------------------------------- memory */
#define GFP_KERNEL 0x1u
#define GFP_NOFS 0x2u
#define GFP_NOIO 0x4u
#define GFP_ATOMIC 0x8u
#define GFP_USER 0x10u
#define GFP_HIGHUSER 0x20u
#define __GFP_HIGHMEM 0x40u
#define __GFP_NOFAIL 0x80u
#define __GFP_ZERO 0x100u
#define __GFP_NOWARN 0x200u
#define __GFP_FS 0x400u
#define __GFP_IO 0x800u
#define __GFP_RECLAIM 0x1000u
#define __GFP_MOVABLE 0x2000u
#define GFP_NOWAIT 0x4000u
#define SLAB_RECLAIM_ACCOUNT 1
#define SLAB_HWCACHE_ALIGN 2
#define SLAB_MEM_SPREAD 4
#define SLAB_ACCOUNT 8
#define SLAB_PANIC 16
void *kmalloc(size_t n, gfp_t g);
void *kzalloc(size_t n, gfp_t g);
void *kcalloc(size_t n, size_t s, gfp_t g);
void *kmalloc_array(size_t n, size_t s, gfp_t g);
void *krealloc(const void *p, size_t n, gfp_t g);
void kfree(const void *p);
#define kvmalloc kmalloc
#define kvzalloc kzalloc
#define kvcalloc kcalloc
#define kvmalloc_array kmalloc_array
#define kvfree kfree
#define vfree kfree
#define vmalloc(n) kmalloc(n, GFP_KERNEL)
#define vzalloc(n) kzalloc(n, GFP_KERNEL)
#define __vmalloc(n, g) kmalloc(n, g)
#define kfree_sensitive kfree
#define kvmemdup kmemdup
static inline void *kvrealloc(const void *p, size_t n, gfp_t g) { return krealloc(p, n, g); }
#define kmalloc_obj(v, ...) ((__typeof__(v) *)kmalloc(sizeof(v), GFP_KERNEL))
#define kzalloc_obj(v, ...) ((__typeof__(v) *)kzalloc(sizeof(v), GFP_KERNEL))
#define kvzalloc_obj kzalloc_obj
#define kmalloc_objs(v, n, ...) ((__typeof__(v) *)kcalloc(n, sizeof(v), GFP_KERNEL))
#define kzalloc_objs(v, n, ...) ((__typeof__(v) *)kcalloc(n, sizeof(v), GFP_KERNEL))
#define kvzalloc_objs kzalloc_objs
static inline bool is_vmalloc_addr(const void *p) { (void)p; return false; }
unsigned long totalram_pages(void);
struct kmem_cache { size_t size; const char *name; void (*ctor)(void *); };
struct kmem_cache *kmem_cache_create(const char *name, unsigned int size,
		unsigned int align, unsigned long flags, void (*ctor)(void *));
void kmem_cache_destroy(struct kmem_cache *c);
void *kmem_cache_alloc(struct kmem_cache *c, gfp_t g);
void *kmem_cache_zalloc(struct kmem_cache *c, gfp_t g);
void kmem_cache_free(struct kmem_cache *c, void *p);
unsigned long __get_free_page(gfp_t g);
void free_page(unsigned long p);
static inline unsigned int memalloc_nofs_save(void) { return 0; }
static inline void memalloc_nofs_restore(unsigned int f) { (void)f; }
static inline unsigned int memalloc_noio_save(void) { return 0; }
static inline void memalloc_noio_restore(unsigned int f) { (void)f; }
void *vmap(void *pages, unsigned int count, unsigned long flags, int prot);
void vunmap(const void *addr);
#define VM_MAP 0
#define PAGE_KERNEL 0
static inline void invalidate_kernel_vmap_range(void *a, int s) { (void)a; (void)s; }
static inline void flush_kernel_vmap_range(void *a, int s) { (void)a; (void)s; }

/* --------------------------------------------------------------- atomics */
typedef struct { volatile int counter; } atomic_t;
typedef struct { volatile s64 counter; } atomic64_t;
typedef struct { volatile int refs; } refcount_t;
#define ATOMIC_INIT(i) { (i) }
#define KA_ADD(p, v) __atomic_add_fetch(p, v, __ATOMIC_SEQ_CST)
#define atomic_read(v) __atomic_load_n(&(v)->counter, __ATOMIC_SEQ_CST)
#define atomic_set(v, i) __atomic_store_n(&(v)->counter, (i), __ATOMIC_SEQ_CST)
#define atomic_inc(v) ((void)KA_ADD(&(v)->counter, 1))
#define atomic_dec(v) ((void)KA_ADD(&(v)->counter, -1))
#define atomic_add(i, v) ((void)KA_ADD(&(v)->counter, (i)))
#define atomic_sub(i, v) ((void)KA_ADD(&(v)->counter, -(i)))
#define atomic_inc_return(v) KA_ADD(&(v)->counter, 1)
#define atomic_dec_return(v) KA_ADD(&(v)->counter, -1)
#define atomic_dec_and_test(v) (KA_ADD(&(v)->counter, -1) == 0)
static inline int atomic_inc_not_zero(atomic_t *v)
{
	int c = atomic_read(v);
	while (c && !__atomic_compare_exchange_n(&v->counter, &c, c + 1, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
		;
	return c != 0;
}
/* 64-bit atomics go through one shim spin lock: i386 has no plain 64-bit atomic add. */
s64 kshim_atomic64_add(atomic64_t *v, s64 d);
#define atomic64_read(v) kshim_atomic64_add(v, 0)
void kshim_atomic64_set(atomic64_t *v, s64 i);
#define atomic64_set(v, i) kshim_atomic64_set(v, i)
#define atomic64_add(i, v) ((void)kshim_atomic64_add(v, (i)))
#define atomic64_sub(i, v) ((void)kshim_atomic64_add(v, -(s64)(i)))
#define atomic64_inc(v) ((void)kshim_atomic64_add(v, 1))
#define atomic64_dec(v) ((void)kshim_atomic64_add(v, -1))
#define atomic64_add_return(i, v) kshim_atomic64_add(v, (i))
#define atomic64_sub_return(i, v) kshim_atomic64_add(v, -(s64)(i))
#define atomic_long_t atomic64_t
#define atomic_long_read atomic64_read

/* --------------------------------------------------------------- bitops */
#define BITS_TO_LONGS(n) DIV_ROUND_UP(n, BITS_PER_LONG)
#define DECLARE_BITMAP(name, bits) unsigned long name[BITS_TO_LONGS(bits)]
static inline int test_bit(long nr, const volatile unsigned long *a)
{ return (a[nr / BITS_PER_LONG] >> (nr % BITS_PER_LONG)) & 1; }
static inline void set_bit(long nr, volatile unsigned long *a)
{ __atomic_fetch_or(&a[nr / BITS_PER_LONG], 1UL << (nr % BITS_PER_LONG), __ATOMIC_SEQ_CST); }
static inline void clear_bit(long nr, volatile unsigned long *a)
{ __atomic_fetch_and(&a[nr / BITS_PER_LONG], ~(1UL << (nr % BITS_PER_LONG)), __ATOMIC_SEQ_CST); }
#define __set_bit set_bit
#define __clear_bit clear_bit
static inline int test_and_set_bit(long nr, volatile unsigned long *a)
{ unsigned long m = 1UL << (nr % BITS_PER_LONG); return (__atomic_fetch_or(&a[nr / BITS_PER_LONG], m, __ATOMIC_SEQ_CST) & m) != 0; }
static inline int test_and_clear_bit(long nr, volatile unsigned long *a)
{ unsigned long m = 1UL << (nr % BITS_PER_LONG); return (__atomic_fetch_and(&a[nr / BITS_PER_LONG], ~m, __ATOMIC_SEQ_CST) & m) != 0; }
#define clear_bit_unlock clear_bit
#define test_and_set_bit_lock test_and_set_bit
unsigned long find_next_bit(const unsigned long *a, unsigned long size, unsigned long off);
unsigned long find_next_zero_bit(const unsigned long *a, unsigned long size, unsigned long off);
#define find_first_bit(a, s) find_next_bit(a, s, 0)
#define find_first_zero_bit(a, s) find_next_zero_bit(a, s, 0)
unsigned int bitmap_weight(const unsigned long *a, unsigned int bits);
#define ffs(x) __builtin_ffs(x)

/* ------------------------------------------------------------------ locks */
/*
 * Real NT-backed locks.  spinlock_t is a KSPIN_LOCK (raises to DISPATCH_LEVEL,
 * the IRQL to restore is kept in the lock since only one holder exists).
 * rwlock_t spins with a yield at the caller's IRQL because fs/ntfs nests
 * read_lock on size_lock.  mutex is a synchronization KEVENT (no owner, not
 * recursive: Linux semantics); rw_semaphore is a counter pair guarded by a
 * spin lock with a notification KEVENT for waiters.  Static initializers are
 * lazily completed on first use.
 */
struct lock_class_key { int dummy; };
typedef struct { uintptr_t lock; unsigned char irql; } spinlock_t;
typedef struct { volatile long cnt; } rwlock_t;
struct mutex { struct ngos_ev ev; volatile int state; };
struct rw_semaphore { uintptr_t sl; int readers; int writer; struct ngos_ev wake; volatile int state; };
typedef struct { unsigned seq; } seqlock_t;
#define DEFINE_MUTEX(m) struct mutex m = { .state = 0 }
#define DEFINE_SPINLOCK(s) spinlock_t s = { 0, 0 }
#define DECLARE_RWSEM(s) struct rw_semaphore s = { .state = 0 }
#define __MUTEX_INITIALIZER(m) { .state = 0 }
void mutex_init(struct mutex *m);
void mutex_lock(struct mutex *m);
int mutex_trylock(struct mutex *m);
void mutex_unlock(struct mutex *m);
int mutex_is_locked(struct mutex *m);
#define mutex_lock_nested(m, c) mutex_lock(m)
#define mutex_lock_interruptible(m) (mutex_lock(m), 0)
#define mutex_destroy(m) do { } while (0)
#define spin_lock_init(s) ((s)->lock = 0, (s)->irql = 0)
static inline void spin_lock(spinlock_t *s) { unsigned char o = ngos_spin_lock(&s->lock); s->irql = o; }
static inline void spin_unlock(spinlock_t *s) { ngos_spin_unlock(&s->lock, s->irql); }
#define spin_lock_irqsave(s, f) do { (f) = 0; spin_lock(s); } while (0)
#define spin_unlock_irqrestore(s, f) do { (void)(f); spin_unlock(s); } while (0)
#define spin_lock_irq spin_lock
#define spin_unlock_irq spin_unlock
#define assert_spin_locked(s) do { (void)(s); } while (0)
#define rwlock_init(l) ((l)->cnt = 0)
void read_lock(rwlock_t *l);
void read_unlock(rwlock_t *l);
void write_lock(rwlock_t *l);
void write_unlock(rwlock_t *l);
#define read_lock_irqsave(l, f) do { (f) = 0; read_lock(l); } while (0)
#define read_unlock_irqrestore(l, f) do { (void)(f); read_unlock(l); } while (0)
#define write_lock_irqsave(l, f) do { (f) = 0; write_lock(l); } while (0)
#define write_unlock_irqrestore(l, f) do { (void)(f); write_unlock(l); } while (0)
void init_rwsem(struct rw_semaphore *s);
void down_read(struct rw_semaphore *s);
void up_read(struct rw_semaphore *s);
void down_write(struct rw_semaphore *s);
void up_write(struct rw_semaphore *s);
int down_read_trylock(struct rw_semaphore *s);
int down_write_trylock(struct rw_semaphore *s);
void downgrade_write(struct rw_semaphore *s);
int rwsem_is_locked(struct rw_semaphore *s);
#define down_read_nested(s, c) down_read(s)
#define down_write_nested(s, c) down_write(s)
#define lockdep_assert_held(l) do { (void)(l); } while (0)
#define lockdep_assert_held_write(l) do { (void)(l); } while (0)
#define lockdep_off() do { } while (0)
#define lockdep_on() do { } while (0)
#define lockdep_set_class(l, k) do { (void)(k); } while (0)
#define lockdep_set_class_and_name(l, k, n) do { } while (0)
#define rcu_read_lock() do { } while (0)
#define rcu_read_unlock() do { } while (0)
#define rcu_barrier() do { } while (0)
#define synchronize_rcu() do { } while (0)

/* ------------------------------------------------------------ wait/work */
typedef struct { int dummy; } wait_queue_head_t;
#define init_waitqueue_head(w) do { (void)(w); } while (0)
#define DECLARE_WAIT_QUEUE_HEAD(w) wait_queue_head_t w
#define wait_event(w, c) do { while (!(c)) ngos_yield(); } while (0)
#define wait_event_interruptible(w, c) ({ wait_event(w, c); 0; })
#define wake_up(w) do { } while (0)
#define wake_up_all(w) do { } while (0)
struct work_struct;
typedef void (*work_func_t)(struct work_struct *);
struct work_struct { work_func_t func; };
struct delayed_work { struct work_struct work; };
struct workqueue_struct { int dummy; };
#define INIT_WORK(w, f) ((w)->func = (f))
bool queue_work(struct workqueue_struct *wq, struct work_struct *w);
bool schedule_work(struct work_struct *w);
bool cancel_work_sync(struct work_struct *w);
void flush_work(struct work_struct *w);
struct workqueue_struct *alloc_workqueue(const char *fmt, unsigned int flags, int max, ...);
void destroy_workqueue(struct workqueue_struct *wq);
void flush_workqueue(struct workqueue_struct *wq);
#define WQ_MEM_RECLAIM 1
#define WQ_FREEZABLE 2
#define WQ_UNBOUND 4
#define WQ_PERCPU 8
static inline int signal_pending(void *t) { (void)t; return 0; }
struct task_struct { int pid; void *journal_info; };
extern struct task_struct kshim_task;
#define current (&kshim_task)
static inline int fatal_signal_pending(void *t) { (void)t; return 0; }
static inline void msleep(unsigned int ms) { ngos_sleep_ms(ms); }
static inline void congestion_wait(int a, int b) { (void)a; (void)b; }
static inline int smp_processor_id(void) { return 0; }

/* ----------------------------------------------------------------- lists */
struct list_head { struct list_head *next, *prev; };
struct hlist_node { struct hlist_node *next, **pprev; };
struct hlist_head { struct hlist_node *first; };
#define LIST_HEAD_INIT(n) { &(n), &(n) }
#define LIST_HEAD(n) struct list_head n = LIST_HEAD_INIT(n)
static inline void INIT_LIST_HEAD(struct list_head *l) { l->next = l->prev = l; }
static inline void __list_add(struct list_head *n, struct list_head *p, struct list_head *x)
{ x->prev = n; n->next = x; n->prev = p; p->next = n; }
static inline void list_add(struct list_head *n, struct list_head *h) { __list_add(n, h, h->next); }
static inline void list_add_tail(struct list_head *n, struct list_head *h) { __list_add(n, h->prev, h); }
static inline void list_del(struct list_head *e)
{ e->next->prev = e->prev; e->prev->next = e->next; e->next = e->prev = NULL; }
static inline void list_del_init(struct list_head *e)
{ e->next->prev = e->prev; e->prev->next = e->next; INIT_LIST_HEAD(e); }
static inline int list_empty(const struct list_head *h) { return h->next == h; }
#define list_entry(p, t, m) container_of(p, t, m)
#define list_first_entry(p, t, m) list_entry((p)->next, t, m)
#define list_last_entry(p, t, m) list_entry((p)->prev, t, m)
#define list_next_entry(pos, m) list_entry((pos)->m.next, __typeof__(*(pos)), m)
#define list_for_each(p, h) for (p = (h)->next; p != (h); p = p->next)
#define list_for_each_safe(p, n, h) for (p = (h)->next, n = p->next; p != (h); p = n, n = p->next)
#define list_for_each_entry(pos, h, m) \
	for (pos = list_first_entry(h, __typeof__(*pos), m); &pos->m != (h); pos = list_next_entry(pos, m))
#define list_for_each_entry_safe(pos, n, h, m) \
	for (pos = list_first_entry(h, __typeof__(*pos), m), n = list_next_entry(pos, m); \
	     &pos->m != (h); pos = n, n = list_next_entry(n, m))

/* ---------------------------------------------------------------- rbtree */
struct rb_node { uintptr_t __rb_parent_color; struct rb_node *rb_right, *rb_left; };
struct rb_root { struct rb_node *rb_node; };
#define RB_ROOT (struct rb_root){ NULL }
#define RB_EMPTY_ROOT(r) ((r)->rb_node == NULL)
#define rb_entry(p, t, m) container_of(p, t, m)
void rb_insert_color(struct rb_node *n, struct rb_root *r);
void rb_erase(struct rb_node *n, struct rb_root *r);
struct rb_node *rb_first(const struct rb_root *r);
struct rb_node *rb_next(const struct rb_node *n);
static inline void rb_link_node(struct rb_node *n, struct rb_node *parent, struct rb_node **link)
{ n->__rb_parent_color = (uintptr_t)parent; n->rb_left = n->rb_right = NULL; *link = n; }

struct va_format { const char *fmt; va_list *va; };
/* ------------------------------------------------------------------ time */
struct timespec64 { s64 tv_sec; long tv_nsec; };
void ktime_get_coarse_real_ts64(struct timespec64 *ts);
void ktime_get_real_ts64(struct timespec64 *ts);
s64 ktime_get_real_seconds(void);
#define NSEC_PER_SEC 1000000000L
#define HZ 100
#define jiffies ngos_jiffies()
static inline struct timespec64 timespec64_trunc(struct timespec64 t, u32 g) { (void)g; return t; }

/* ----------------------------------------------------------------- module */
struct module { int dummy; };
#define THIS_MODULE ((struct module *)0)
#define MODULE_AUTHOR(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_LICENSE(x)
#define MODULE_ALIAS_FS(x)
#define MODULE_PARM_DESC(a, b)
#define MODULE_IMPORT_NS(x)
#define module_param(n, t, p)
typedef int (*initcall_t)(void);
typedef void (*exitcall_t)(void);
#define module_init(fn) initcall_t kshim_module_init = fn;
#define module_exit(fn) exitcall_t kshim_module_exit = fn;

/* -------------------------------------------------------------- uid/gid */
struct user_namespace { int dummy; };
extern struct user_namespace init_user_ns;
#define GLOBAL_ROOT_UID ((kuid_t){ 0 })
#define GLOBAL_ROOT_GID ((kgid_t){ 0 })
#define INVALID_UID ((kuid_t){ (u32)-1 })
#define INVALID_GID ((kgid_t){ (u32)-1 })
static inline kuid_t make_kuid(struct user_namespace *ns, u32 u) { (void)ns; return (kuid_t){ u }; }
static inline kgid_t make_kgid(struct user_namespace *ns, u32 g) { (void)ns; return (kgid_t){ g }; }
static inline u32 from_kuid(struct user_namespace *ns, kuid_t u) { (void)ns; return u.val; }
static inline u32 from_kgid(struct user_namespace *ns, kgid_t g) { (void)ns; return g.val; }
static inline u32 from_kuid_munged(struct user_namespace *ns, kuid_t u) { (void)ns; return u.val; }
static inline u32 from_kgid_munged(struct user_namespace *ns, kgid_t g) { (void)ns; return g.val; }
static inline bool uid_valid(kuid_t u) { return u.val != (u32)-1; }
static inline bool gid_valid(kgid_t g) { return g.val != (u32)-1; }
static inline bool uid_eq(kuid_t a, kuid_t b) { return a.val == b.val; }
static inline bool gid_eq(kgid_t a, kgid_t b) { return a.val == b.val; }
static inline struct user_namespace *current_user_ns(void) { return &init_user_ns; }
#define current_fsuid() GLOBAL_ROOT_UID
#define current_fsgid() GLOBAL_ROOT_GID
static inline bool capable(int c) { (void)c; return true; }
#define CAP_SYS_ADMIN 21
#define CAP_LINUX_IMMUTABLE 9
#define CAP_FOWNER 3
#define CAP_SYS_RESOURCE 24

#define wchar_t wchar_t_
#include "kshim_vfs.h"

#endif /* KSHIM_H */
