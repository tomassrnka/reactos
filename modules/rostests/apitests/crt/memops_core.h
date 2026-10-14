/*
 * PROJECT:         ReactOS tests
 * LICENSE:         LGPLv2.1+ - See COPYING.LIB in the top level directory
 * PURPOSE:         Shared body of the memcpy, memmove and memset tests (user mode and kernel mode)
 *
 * The includer defines MEMOPS_LOG(...) and provides ok().
 */

typedef void *(__cdecl *PFN_COPY)(void *, const void *, size_t);
typedef void *(__cdecl *PFN_SET)(void *, int, size_t);
typedef void (__stdcall *PFN_RTLMOVE)(void *, const void *, size_t);
typedef void (__stdcall *PFN_RTLFILL)(void *, size_t, unsigned char);

#define SMALL 600
#define BIG (200 * 1024)

static unsigned int g_seed;
static int g_fails;

static unsigned char Rnd8(void)
{
    g_seed = g_seed * 1664525U + 1013904223U;
    return (unsigned char)(g_seed >> 24);
}

static void RefCopy(unsigned char *d, const unsigned char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) d[i] = s[i];
}

static void Randomize(unsigned char *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) p[i] = Rnd8();
}

#define MEMOPS_FAIL(...) do { if (g_fails++ < 6) { MEMOPS_LOG(__VA_ARGS__); } } while (0)

/* Non-overlapping copy: every source and destination alignment, every length from 0 to 300 */
static void Test_Copy(const char *name, PFN_COPY copy)
{
    static unsigned char src0[SMALL], src[SMALL], dst0[SMALL], dst[SMALL];
    unsigned sa, da;
    size_t len, i;

    g_fails = 0;
    g_seed = 42;
    Randomize(src0, SMALL);
    Randomize(dst0, SMALL);
    for (sa = 0; sa < 16; sa++)
    {
        for (da = 0; da < 16; da++)
        {
            for (len = 0; len <= 300; len++)
            {
                void *ret;

                RefCopy(src, src0, SMALL);
                RefCopy(dst, dst0, SMALL);
                ret = copy(dst + 16 + da, src + 16 + sa, len);
                if (ret != dst + 16 + da)
                    MEMOPS_FAIL("%s: wrong return value, sa %u da %u len %u\n", name, sa, da, (unsigned)len);
                for (i = 0; i < SMALL; i++)
                {
                    unsigned char want = dst0[i];
                    if (i >= 16 + da && i < 16 + da + len) want = src0[16 + sa + (i - 16 - da)];
                    if (dst[i] != want)
                    {
                        MEMOPS_FAIL("%s: sa %u da %u len %u: dst byte %u is %02x, expected %02x\n",
                               name, sa, da, (unsigned)len, (unsigned)i, dst[i], want);
                        break;
                    }
                }
                for (i = 0; i < SMALL; i++)
                {
                    if (src[i] != src0[i])
                    {
                        MEMOPS_FAIL("%s: sa %u da %u len %u: source byte %u was modified\n", name, sa, da, (unsigned)len, (unsigned)i);
                        break;
                    }
                }
            }
        }
    }
    ok(g_fails == 0, "%s: %d failure(s)\n", name, g_fails);
}

static void MoveCheck(const char *name, PFN_COPY move, PFN_RTLMOVE rtlmove, unsigned char *buf, unsigned char *orig,
                      unsigned char *tmp, size_t region, size_t sofs, int delta, size_t len)
{
    size_t dofs = (size_t)((long)sofs + delta), i;
    void *ret = NULL;

    RefCopy(buf, orig, region);
    RefCopy(tmp, orig + sofs, len);
    if (move) ret = move(buf + dofs, buf + sofs, len);
    else rtlmove(buf + dofs, buf + sofs, len);
    if (move && ret != buf + dofs)
        MEMOPS_FAIL("%s: wrong return value, src %u delta %d len %u\n", name, (unsigned)sofs, delta, (unsigned)len);
    for (i = 0; i < region; i++)
    {
        unsigned char want = orig[i];
        if (i >= dofs && i < dofs + len) want = tmp[i - dofs];
        if (buf[i] != want)
        {
            MEMOPS_FAIL("%s: src %u delta %d len %u: byte %u is %02x, expected %02x\n",
                   name, (unsigned)sofs, delta, (unsigned)len, (unsigned)i, buf[i], want);
            return;
        }
    }
}

/* Overlapping copy in both directions: every length from 0 to 300, offsets of -24 to 24 bytes and a few farther ones */
static void Test_Move(const char *name, PFN_COPY move, PFN_RTLMOVE rtlmove)
{
    static unsigned char buf[SMALL + 64], orig[SMALL + 64], tmp[SMALL + 64];
    static const int deltas[] = { -100, -64, -33, -32, -31, 0 };
    unsigned sa;
    int delta, k;
    size_t len;

    g_fails = 0;
    g_seed = 7;
    Randomize(orig, sizeof(orig));
    for (sa = 0; sa < 8; sa++)
    {
        for (len = 0; len <= 300; len++)
        {
            for (delta = -24; delta <= 24; delta++)
                MoveCheck(name, move, rtlmove, buf, orig, tmp, SMALL, 150 + sa, delta, len);
            for (k = 0; k < (int)(sizeof(deltas) / sizeof(deltas[0])); k++)
            {
                MoveCheck(name, move, rtlmove, buf, orig, tmp, SMALL, 150 + sa, deltas[k], len);
                if (deltas[k]) MoveCheck(name, move, rtlmove, buf, orig, tmp, SMALL, 150 + sa, -deltas[k], len);
            }
        }
    }
    ok(g_fails == 0, "%s (overlap, short): %d failure(s)\n", name, g_fails);
}

/* Large blocks around the sizes where an implementation might change strategy */
static void Test_MoveBig(const char *name, PFN_COPY move, PFN_RTLMOVE rtlmove)
{
    static unsigned char buf[BIG], orig[BIG], tmp[BIG];
    static const size_t lens[] = { 255, 256, 257, 511, 512, 513, 1023, 1024, 1025, 4095, 4096, 4097, 65535, 65536, 65537 };
    static const unsigned offs[] = { 0, 1, 3, 5, 7, 8, 15 };
    static const int deltas[] = { -4097, -4096, -65, -9, -8, -7, -1, 1, 7, 8, 9, 65, 4096, 4097 };
    unsigned i, j, k;

    g_fails = 0;
    g_seed = 99;
    Randomize(orig, BIG);
    for (i = 0; i < sizeof(lens) / sizeof(lens[0]); i++)
        for (j = 0; j < sizeof(offs) / sizeof(offs[0]); j++)
            for (k = 0; k < sizeof(deltas) / sizeof(deltas[0]); k++)
                MoveCheck(name, move, rtlmove, buf, orig, tmp, 100000, 8200 + offs[j], deltas[k], lens[i]);
    ok(g_fails == 0, "%s (overlap, large): %d failure(s)\n", name, g_fails);
}

static void Test_Set(const char *name, PFN_SET set, PFN_RTLFILL fill)
{
    static unsigned char buf[SMALL];
    static const int vals[] = { 0, 0x5A, 0xFF, 0x1A5, -1, 0x80, 0x7F };
    unsigned da, v;
    size_t len, i;

    g_fails = 0;
    for (v = 0; v < sizeof(vals) / sizeof(vals[0]); v++)
    {
        for (da = 0; da < 16; da++)
        {
            for (len = 0; len <= 300; len++)
            {
                unsigned char val = (unsigned char)vals[v];
                void *ret = NULL;

                for (i = 0; i < SMALL; i++) buf[i] = (unsigned char)(i * 7 + 1);
                if (set) ret = set(buf + 16 + da, vals[v], len);
                else fill(buf + 16 + da, len, val);
                if (set && ret != buf + 16 + da)
                    MEMOPS_FAIL("%s: wrong return value, da %u len %u\n", name, da, (unsigned)len);
                for (i = 0; i < SMALL; i++)
                {
                    unsigned char want = (unsigned char)(i * 7 + 1);
                    if (i >= 16 + da && i < 16 + da + len) want = val;
                    if (buf[i] != want)
                    {
                        MEMOPS_FAIL("%s: val %02x da %u len %u: byte %u is %02x, expected %02x\n",
                               name, val, da, (unsigned)len, (unsigned)i, buf[i], want);
                        break;
                    }
                }
            }
        }
    }
    ok(g_fails == 0, "%s: %d failure(s)\n", name, g_fails);
}

