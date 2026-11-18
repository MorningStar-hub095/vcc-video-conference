/*【2026-11-18 审查修复-C1】服务端标准 MD5 实现（RFC 1321）
 * 实现: 单块状态机 + 标准 K/S 表 + g 索引公式，输出小写十六进制。
 * 验证: 通过 RFC 1321 标准向量（见 md5_util.h 头注释与部署冒烟）：
 *       ""               -> d41d8cd98f00b204e9800998ecf8427e
 *       "abc"            -> 900150983cd24fb0d6963f7d28e17f72
 *       "message digest" -> f96b697d7cb7938d525a2f31aaf161d0
 *       64×'a'           -> 57edf4a22be3c955ac49da2e2107b67a
 * 历史: 初版（本文件尾部 #if 0 注释保留）为客户端实现的移植，向量验证错误，已弃用。
 */
#include "md5_util.h"
#include <string.h>
#include <stdint.h>

using namespace std;

namespace {

struct MD5Ctx
{
    uint32_t a, b, c, d;        //链接变量
    uint64_t nbits;             //已处理比特数
    unsigned char buf[64];      //输入缓冲
    size_t   buflen;            //缓冲内字节数
    unsigned char digest[16];   //摘要
};

inline uint32_t md5_rol(uint32_t x, int s)
{
    return (x << s) | (x >> (32 - s));
}

//64 个轮常数 K[i] = floor(abs(sin(i+1)) * 2^32)（RFC 1321 标准，逐项与 T 表一致）
const uint32_t MD5_K[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeEu,
    0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
    0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
    0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
    0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
    0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
    0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
    0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
    0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
    0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
    0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
    0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
    0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
    0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u
};

//每轮移位量
const int MD5_S[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
};

void md5_transform(MD5Ctx* ctx, const unsigned char* p)
{
    uint32_t M[16];
    for( int i = 0; i < 16; ++i )
    {
        //小端取 32 位字
        M[i] = (uint32_t)p[i * 4]
             | ((uint32_t)p[i * 4 + 1] << 8)
             | ((uint32_t)p[i * 4 + 2] << 16)
             | ((uint32_t)p[i * 4 + 3] << 24);
    }
    uint32_t A = ctx->a, B = ctx->b, C = ctx->c, D = ctx->d;
    for( int i = 0; i < 64; ++i )
    {
        uint32_t F;
        int g;
        if( i < 16 )      { F = (B & C) | (~B & D);        g = i; }
        else if( i < 32 ) { F = (D & B) | (~D & C);        g = (5 * i + 1) % 16; }
        else if( i < 48 ) { F = B ^ C ^ D;                 g = (3 * i + 5) % 16; }
        else              { F = C ^ (B | ~D);              g = (7 * i) % 16; }
        uint32_t tmp = D;
        D = C;
        C = B;
        B = B + md5_rol( A + F + MD5_K[i] + M[g], MD5_S[i] );
        A = tmp;
    }
    ctx->a += A;
    ctx->b += B;
    ctx->c += C;
    ctx->d += D;
}

void md5_update(MD5Ctx* ctx, const unsigned char* p, size_t n)
{
    ctx->nbits += (uint64_t)n * 8;
    while( n > 0 )
    {
        if( ctx->buflen == 0 && n >= 64 )
        {
            md5_transform( ctx, p );
            p += 64;
            n -= 64;
            continue;
        }
        size_t take = 64 - ctx->buflen;
        if( take > n ) take = n;
        memcpy( ctx->buf + ctx->buflen, p, take );
        ctx->buflen += take;
        p += take;
        n -= take;
        if( ctx->buflen == 64 )
        {
            md5_transform( ctx, ctx->buf );
            ctx->buflen = 0;
        }
    }
}

void md5_final(MD5Ctx* ctx)
{
    unsigned char pad[72];
    size_t padLen = ( ctx->buflen < 56 ) ? ( 56 - ctx->buflen ) : ( 120 - ctx->buflen );
    pad[0] = 0x80;
    for( size_t i = 1; i < padLen; ++i ) pad[i] = 0;
    //64 位小端比特长度
    unsigned char lenb[8];
    uint64_t bits = ctx->nbits;
    for( int i = 0; i < 8; ++i ) lenb[i] = (unsigned char)( ( bits >> ( 8 * i ) ) & 0xff );
    md5_update( ctx, pad, padLen );        //注: nbits 会多计填充长度，但下面直接用先前快照 lenb，不受影响
    md5_update( ctx, lenb, 8 );
    //小端输出四个链接变量
    uint32_t st[4] = { ctx->a, ctx->b, ctx->c, ctx->d };
    for( int i = 0; i < 4; ++i )
    {
        ctx->digest[i * 4 + 0] = (unsigned char)( st[i] & 0xff );
        ctx->digest[i * 4 + 1] = (unsigned char)( ( st[i] >> 8 ) & 0xff );
        ctx->digest[i * 4 + 2] = (unsigned char)( ( st[i] >> 16 ) & 0xff );
        ctx->digest[i * 4 + 3] = (unsigned char)( ( st[i] >> 24 ) & 0xff );
    }
}

} // namespace

std::string md5_hex(const std::string& src)
{
    MD5Ctx ctx;
    ctx.a = 0x67452301u;
    ctx.b = 0xefcdab89u;
    ctx.c = 0x98badcfeu;
    ctx.d = 0x10325476u;
    ctx.nbits = 0;
    ctx.buflen = 0;
    md5_update( &ctx, (const unsigned char*)src.data(), src.size() );
    md5_final( &ctx );
    static const char* HEX = "0123456789abcdef";
    std::string out;
    out.reserve( 32 );
    for( int i = 0; i < 16; ++i )
    {
        out += HEX[ ( ctx.digest[i] >> 4 ) & 0x0f ];
        out += HEX[ ctx.digest[i] & 0x0f ];
    }
    return out;
}

/*================ 初版实现（已弃用，原样注释保留） ================
 * 弃用原因（2026-11-18）: 本版为客户端 vcc-client/md5/md5.cpp 的 Linux 适配
 * （ulong→4 字节），经 RFC 1321 标准向量验证输出错误：
 *   "abc"  实测 90563ebc218d3c0c07f3b07e2e0d1416 / 应为 900150983cd24fb0d6963f7d28e17f72
 *   空串   实测 06542ed15ef489c6d6d4582ee4c79a7f / 应为 d41d8cd98f00b204e9800998ecf8427e
 * 根因追查时同时发现客户端原版 md5 类同样不标准（两端从未独立验证）。
 * 按保留历史原则整块原文注释保留在本文件末尾的 #if 0 块中，勿启用。
 ================= 初版实现注释保留结束 ================ */

#if 0
/*=============== 初版错误实现原文（#if 0 注释保留，勿启用）===============
 * 2026-11-18 弃用：RFC 1321 向量验证错误（见文件头注释）。
 * 下文为该初版的逐字原文（客户端 md5.cpp 的 Linux 适配版）。
========================================================================= */
#include <string.h>

namespace {

typedef unsigned char md5_byte;

struct MD5Ctx
{
    md5_uint32 _state[4];
    md5_uint32 _count[2];
    md5_byte _buffer[64];
    md5_byte _digest[16];
    bool _finished;
};

const md5_byte PADDING[64] = { 0x80 };
const char HEX[16] = {
    '0', '1', '2', '3',
    '4', '5', '6', '7',
    '8', '9', 'a', 'b',
    'c', 'd', 'e', 'f'
};

void ctx_reset(MD5Ctx* c)
{
    c->_finished = false;
    c->_count[0] = c->_count[1] = 0;
    c->_state[0] = 0x67452301;
    c->_state[1] = 0xefcdab89;
    c->_state[2] = 0x98badcfe;
    c->_state[3] = 0x10325476;
}

void ctx_encode(const md5_uint32 *input, md5_byte *output, size_t length)
{
    for(size_t i=0, j=0; j<length; i++, j+=4) {
        output[j]   = (md5_byte)(input[i] & 0xff);
        output[j+1] = (md5_byte)((input[i] >> 8) & 0xff);
        output[j+2] = (md5_byte)((input[i] >> 16) & 0xff);
        output[j+3] = (md5_byte)((input[i] >> 24) & 0xff);
    }
}

void ctx_decode(const md5_byte *input, md5_uint32 *output, size_t length)
{
    for(size_t i=0, j=0; j<length; i++, j+=4) {
        output[i] = ((md5_uint32)input[j]) | (((md5_uint32)input[j+1]) << 8) |
            (((md5_uint32)input[j+2]) << 16) | (((md5_uint32)input[j+3]) << 24);
    }
}

void ctx_transform(MD5Ctx* c, const md5_byte block[64])
{
    md5_uint32 a = c->_state[0], b = c->_state[1], cc = c->_state[2], d = c->_state[3], x[16];

    ctx_decode(block, x, 64);

    /* Round 1 */
    FF (a, b, cc, d, x[ 0], S11, 0xd76aa478);
    FF (d, a, b, cc, x[ 1], S12, 0xe8c7b756);
    FF (cc, d, a, b, x[ 2], S13, 0x242070db);
    FF (b, cc, d, a, x[ 3], S14, 0xc1bdceee);
    FF (a, b, cc, d, x[ 4], S11, 0xf57c0faf);
    FF (d, a, b, cc, x[ 5], S12, 0x4787c62a);
    FF (cc, d, a, b, x[ 6], S13, 0xa8304613);
    FF (b, cc, d, a, x[ 7], S14, 0xfd469501);
    FF (a, b, cc, d, x[ 8], S11, 0x698098d8);
    FF (d, a, b, cc, x[ 9], S12, 0x8b44f7af);
    FF (cc, d, a, b, x[10], S13, 0xffff5bb1);
    FF (b, cc, d, a, x[11], S14, 0x895cd7be);
    FF (a, b, cc, d, x[12], S11, 0x6b901122);
    FF (d, a, b, cc, x[13], S12, 0xfd987193);
    FF (cc, d, a, b, x[14], S13, 0xa679438e);
    FF (b, cc, d, a, x[15], S14, 0x49b40821);

    /* Round 2 */
    GG (a, b, cc, d, x[ 1], S21, 0xf61e2562);
    GG (d, a, b, cc, x[ 6], S22, 0xc040b340);
    GG (cc, d, a, b, x[11], S23, 0x265e5a51);
    GG (b, cc, d, a, x[ 0], S24, 0xe9b6c7aa);
    GG (a, b, cc, d, x[ 5], S21, 0xd62f105d);
    GG (d, a, b, cc, x[10], S22,  0x2441453);
    GG (cc, d, a, b, x[15], S23, 0xd8a1e681);
    GG (b, cc, d, a, x[ 4], S24, 0xe7d3fbc8);
    GG (a, b, cc, d, x[ 9], S21, 0x21e1cde6);
    GG (d, a, b, cc, x[14], S22, 0xc33707d6);
    GG (cc, d, a, b, x[ 3], S23, 0xf4d50d87);
    GG (b, cc, d, a, x[ 8], S24, 0x455a14ed);
    GG (a, b, cc, d, x[13], S21, 0xa9e3e905);
    GG (d, a, b, cc, x[ 2], S22, 0xfcefa3f8);
    GG (cc, d, a, b, x[ 7], S23, 0x676f02d9);
    GG (b, cc, d, a, x[12], S24, 0x8d2a4c8a);

    /* Round 3 */
    HH (a, b, cc, d, x[ 5], S31, 0xfffa3942);
    HH (d, a, b, cc, x[ 8], S32, 0x8771f681);
    HH (cc, d, a, b, x[11], S33, 0x6d9d6122);
    HH (b, cc, d, a, x[14], S34, 0xfde5380c);
    HH (a, b, cc, d, x[ 1], S31, 0xa4beea44);
    HH (d, a, b, cc, x[ 4], S32, 0x4bdecfa9);
    HH (b, cc, d, a, x[ 7], S33, 0xf6bb4b60);
    HH (a, b, cc, d, x[10], S34, 0xbebfbc70);
    HH (d, a, b, cc, x[13], S31, 0x289b7ec6);
    HH (cc, d, a, b, x[ 0], S32, 0xeaa127fa);
    HH (b, cc, d, a, x[ 3], S33, 0xd4ef3085);
    HH (a, b, cc, d, x[ 6], S34,  0x4881d05);
    HH (d, a, b, cc, x[ 9], S31, 0xd9d4d039);
    HH (b, cc, d, a, x[12], S32, 0xe6db99e5);
    HH (cc, d, a, b, x[15], S33, 0x1fa27cf8);
    HH (a, b, cc, d, x[ 2], S34, 0xc4ac5665);

    /* Round 4 */
    II (a, b, cc, d, x[ 0], S41, 0xf4292244);
    II (d, a, b, cc, x[ 7], S42, 0x432aff97);
    II (cc, d, a, b, x[14], S43, 0xab9423a7);
    II (b, cc, d, a, x[ 5], S44, 0xfc93a039);
    II (a, b, cc, d, x[12], S41, 0x655b59c3);
    II (d, a, b, cc, x[ 3], S42, 0x8f0ccc92);
    II (cc, d, a, b, x[10], S43, 0xffeff47d);
    II (b, cc, d, a, x[ 1], S44, 0x85845dd1);
    II (a, b, cc, d, x[ 8], S41, 0x6fa87e4f);
    II (d, a, b, cc, x[15], S42, 0xfe2ce6e0);
    II (cc, d, a, b, x[ 6], S43, 0xa3014314);
    II (b, cc, d, a, x[13], S44, 0x4e0811a1);
    II (a, b, cc, d, x[ 4], S41, 0xf7537e82);
    II (d, a, b, cc, x[11], S42, 0xbd3af235);
    II (cc, d, a, b, x[ 2], S43, 0x2ad7d2bb);
    II (b, cc, d, a, x[ 9], S44, 0xeb86d391);

    c->_state[0] += a;
    c->_state[1] += b;
    c->_state[2] += cc;
    c->_state[3] += d;
}

void ctx_update(MD5Ctx* c, const md5_byte *input, size_t length)
{
    md5_uint32 i, index, partLen;

    c->_finished = false;

    index = (md5_uint32)((c->_count[0] >> 3) & 0x3f);

    if((c->_count[0] += ((md5_uint32)length << 3)) < ((md5_uint32)length << 3))
        c->_count[1]++;
    c->_count[1] += ((md5_uint32)length >> 29);

    partLen = 64 - index;

    if(length >= partLen) {

        memcpy(&c->_buffer[index], input, partLen);
        ctx_transform(c, c->_buffer);

        for (i = partLen; i + 63 < length; i += 64)
            ctx_transform(c, &input[i]);
        index = 0;

    } else {
        i = 0;
    }

    memcpy(&c->_buffer[index], &input[i], length-i);
}

void ctx_final(MD5Ctx* c)
{
    md5_byte bits[8];
    md5_uint32 index, padLen;

    ctx_encode(c->_count, bits, 8);

    index = (md5_uint32)((c->_count[0] >> 3) & 0x3f);
    padLen = (index < 56) ? (56 - index) : (120 - index);
    ctx_update(c, PADDING, padLen);

    ctx_update(c, bits, 8);

    ctx_encode(c->_state, c->_digest, 16);
}

string bytesToHexString(const md5_byte *input, size_t length)
{
    string str;
    str.reserve(length << 1);
    for(size_t i = 0; i < length; i++) {
        int t = input[i];
        int a = t / 16;
        int b = t % 16;
        str.append(1, HEX[a]);
        str.append(1, HEX[b]);
    }
    return str;
}

} // namespace

std::string md5_hex(const std::string& src)
{
    MD5Ctx ctx;
    ctx_reset(&ctx);
    ctx_update(&ctx, (const md5_byte*)src.data(), src.size());
    ctx_final(&ctx);
    return bytesToHexString(ctx._digest, 16);
}

#endif // 初版错误实现注释保留结束
