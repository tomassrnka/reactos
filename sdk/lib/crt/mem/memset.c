
#include <string.h>

#if defined(_MSC_VER) && defined(_M_AMD64)
#include <intrin.h>
#endif

#ifdef _MSC_VER
#pragma function(memset)
#endif /* _MSC_VER */

void* __cdecl memset(void* src, int val, size_t count)
{
    char *char_src = (char *)src;

#if defined(_M_AMD64) || defined(__x86_64__)
    if (count >= 8)
    {
        size_t qwords = count >> 3;
        unsigned long long pattern = (unsigned char)val * 0x0101010101010101ULL;
#ifdef __GNUC__
        __asm__ __volatile__("rep stosq" : "+D"(char_src), "+c"(qwords) : "a"(pattern) : "memory");
#else
        __stosq((unsigned long long *)char_src, pattern, qwords);
        char_src += qwords * 8;
#endif
        count &= 7;
    }
#endif
    while(count>0) {
        *char_src = val;
        char_src++;
        count--;
    }
    return src;
}
