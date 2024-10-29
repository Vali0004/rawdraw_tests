#pragma once

#ifndef NK_INTERN
#define NK_INTERN static
#endif
#ifndef NK_GLOBAL
#define NK_GLOBAL static
#endif

#ifndef NK_ASSERT
#include <assert.h>
#define NK_ASSERT(expr) assert(expr)
#endif

NK_GLOBAL unsigned char* nk__barrier;
NK_GLOBAL unsigned char* nk__barrier2;
NK_GLOBAL unsigned char* nk__barrier3;
NK_GLOBAL unsigned char* nk__barrier4;
NK_GLOBAL unsigned char* nk__dout;
NK_LIB void* nk_memcopy(void* dst0, const void* src0, nk_size length);

NK_INTERN unsigned int
nk_decompress_length(unsigned char* input)
{
    return (unsigned int)((input[8] << 24) + (input[9] << 16) + (input[10] << 8) + input[11]);
}
NK_INTERN void
nk__match(unsigned char* data, unsigned int length)
{
    /* INVERSE of memmove... write each byte before copying the next...*/
    NK_ASSERT(nk__dout + length <= nk__barrier);
    if (nk__dout + length > nk__barrier)
    {
        nk__dout += length; return;
    }
    if (data < nk__barrier4)
    {
        nk__dout = nk__barrier + 1; return;
    }
    while (length--) *nk__dout++ = *data++;
}
NK_INTERN void
nk__lit(unsigned char* data, unsigned int length)
{
    NK_ASSERT(nk__dout + length <= nk__barrier);
    if (nk__dout + length > nk__barrier)
    {
        nk__dout += length; return;
    }
    if (data < nk__barrier2)
    {
        nk__dout = nk__barrier + 1; return;
    }
    nk_memcopy(nk__dout, data, length);
    nk__dout += length;
}
NK_INTERN unsigned char*
nk_decompress_token(unsigned char* i)
{
#define nk__in2(x)   ((i[x] << 8) + i[(x)+1])
#define nk__in3(x)   ((i[x] << 16) + nk__in2((x)+1))
#define nk__in4(x)   ((i[x] << 24) + nk__in3((x)+1))

    if (*i >= 0x20)
    { /* use fewer if's for cases that expand small */
        if (*i >= 0x80)       nk__match(nk__dout - i[1] - 1, (unsigned int)i[0] - 0x80 + 1), i += 2;
        else if (*i >= 0x40)  nk__match(nk__dout - (nk__in2(0) - 0x4000 + 1), (unsigned int)i[2] + 1), i += 3;
        else /* *i >= 0x20 */ nk__lit(i + 1, (unsigned int)i[0] - 0x20 + 1), i += 1 + (i[0] - 0x20 + 1);
    }
    else
    { /* more ifs for cases that expand large, since overhead is amortized */
        if (*i >= 0x18)       nk__match(nk__dout - (unsigned int)(nk__in3(0) - 0x180000 + 1), (unsigned int)i[3] + 1), i += 4;
        else if (*i >= 0x10)  nk__match(nk__dout - (unsigned int)(nk__in3(0) - 0x100000 + 1), (unsigned int)nk__in2(3) + 1), i += 5;
        else if (*i >= 0x08)  nk__lit(i + 2, (unsigned int)nk__in2(0) - 0x0800 + 1), i += 2 + (nk__in2(0) - 0x0800 + 1);
        else if (*i == 0x07)  nk__lit(i + 3, (unsigned int)nk__in2(1) + 1), i += 3 + (nk__in2(1) + 1);
        else if (*i == 0x06)  nk__match(nk__dout - (unsigned int)(nk__in3(1) + 1), i[4] + 1u), i += 5;
        else if (*i == 0x04)  nk__match(nk__dout - (unsigned int)(nk__in3(1) + 1), (unsigned int)nk__in2(4) + 1u), i += 6;
    }
    return i;
}
NK_INTERN unsigned int
nk_adler32(unsigned int adler32, unsigned char* buffer, unsigned int buflen)
{
    const unsigned long ADLER_MOD = 65521;
    unsigned long s1 = adler32 & 0xffff, s2 = adler32 >> 16;
    unsigned long blocklen, i;

    blocklen = buflen % 5552;
    while (buflen)
    {
        for (i = 0; i + 7 < blocklen; i += 8)
        {
            s1 += buffer[0]; s2 += s1;
            s1 += buffer[1]; s2 += s1;
            s1 += buffer[2]; s2 += s1;
            s1 += buffer[3]; s2 += s1;
            s1 += buffer[4]; s2 += s1;
            s1 += buffer[5]; s2 += s1;
            s1 += buffer[6]; s2 += s1;
            s1 += buffer[7]; s2 += s1;
            buffer += 8;
        }
        for (; i < blocklen; ++i)
        {
            s1 += *buffer++; s2 += s1;
        }

        s1 %= ADLER_MOD; s2 %= ADLER_MOD;
        buflen -= (unsigned int)blocklen;
        blocklen = 5552;
    }
    return (unsigned int)(s2 << 16) + (unsigned int)s1;
}
NK_INTERN unsigned int
nk_decompress(unsigned char* output, unsigned char* i, unsigned int length)
{
    unsigned int olen;
    if (nk__in4(0) != 0x57bC0000) return 0;
    if (nk__in4(4) != 0)          return 0; /* error! stream is > 4GB */
    olen = nk_decompress_length(i);
    nk__barrier2 = i;
    nk__barrier3 = i + length;
    nk__barrier = output + olen;
    nk__barrier4 = output;
    i += 16;

    nk__dout = output;
    for (;;)
    {
        unsigned char* old_i = i;
        i = nk_decompress_token(i);
        if (i == old_i)
        {
            if (*i == 0x05 && i[1] == 0xfa)
            {
                NK_ASSERT(nk__dout == output + olen);
                if (nk__dout != output + olen) return 0;
                if (nk_adler32(1, output, olen) != (unsigned int)nk__in4(2))
                    return 0;
                return olen;
            }
            else
            {
                NK_ASSERT(0); /* NOTREACHED */
                return 0;
            }
        }
        NK_ASSERT(nk__dout <= output + olen);
        if (nk__dout > output + olen)
            return 0;
    }
}
NK_INTERN unsigned int
nk_decode_85_byte(char c)
{
    return (unsigned int)((c >= '\\') ? c - 36 : c - 35);
}
NK_INTERN void
nk_decode_85(unsigned char* dst, const unsigned char* src)
{
    while (*src)
    {
        unsigned int tmp =
            nk_decode_85_byte((char)src[0]) +
            85 * (nk_decode_85_byte((char)src[1]) +
                85 * (nk_decode_85_byte((char)src[2]) +
                    85 * (nk_decode_85_byte((char)src[3]) +
                        85 * nk_decode_85_byte((char)src[4]))));

        /* we can't assume little-endianess. */
        dst[0] = (unsigned char)((tmp >> 0) & 0xFF);
        dst[1] = (unsigned char)((tmp >> 8) & 0xFF);
        dst[2] = (unsigned char)((tmp >> 16) & 0xFF);
        dst[3] = (unsigned char)((tmp >> 24) & 0xFF);

        src += 5;
        dst += 4;
    }
}