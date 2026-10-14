#include <string.h>

#if defined(_MSC_VER) && defined(_M_AMD64)
#include <intrin.h>
#endif

#ifdef __GNUC__
typedef unsigned long long __attribute__((__may_alias__, __aligned__(1))) crt_uqword;
#else
typedef unsigned long long crt_uqword;
#endif

#if defined(_MSC_VER) && (_MSC_VER >= 1910 || !defined(_WIN64))
#pragma function(memmove)
#endif /* _MSC_VER */

/* NOTE: This code is duplicated in memcpy function */
void * __cdecl memmove(void *dest,const void *src,size_t count)
{
    char *char_dest = (char *)dest;
    char *char_src = (char *)src;

    if ((char_dest <= char_src) || (char_dest >= (char_src+count)))
    {
        /*  non-overlapping buffers */
#if defined(_M_AMD64) || defined(__x86_64__)
        /* rep movsq moves 8 bytes per step; the byte loop below runs at about 2 GB/s */
        if (count >= 8)
        {
            size_t qwords = count >> 3;
#ifdef __GNUC__
            __asm__ __volatile__("rep movsq" : "+D"(char_dest), "+S"(char_src), "+c"(qwords) : : "memory");
#else
            __movsq((unsigned long long *)char_dest, (const unsigned long long *)char_src, qwords);
            char_dest += qwords * 8;
            char_src += qwords * 8;
#endif
            count &= 7;
        }
#endif
        while(count > 0)
	{
            *char_dest = *char_src;
            char_dest++;
            char_src++;
            count--;
	}
    }
    else
    {
        /* overlaping buffers */
#if defined(_M_AMD64) || defined(__x86_64__)
        /* copy 8 bytes per step from the end; each chunk is read before the chunk below it is written */
        while (count >= 8)
        {
            count -= 8;
            *(crt_uqword *)((char *)dest + count) = *(const crt_uqword *)((const char *)src + count);
        }
#endif
        char_dest = (char *)dest + count - 1;
        char_src = (char *)src + count - 1;

        while(count > 0)
	{
           *char_dest = *char_src;
           char_dest--;
           char_src--;
           count--;
	}
    }

    return dest;
}
