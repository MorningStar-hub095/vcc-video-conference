// ============================================================================
// bcrypt 哈希封装（内置 crypt_blowfish 实现）
//【修复-P0：VM 环境不支持 bcrypt】实现来源切换说明：
// 修改原因: 本封装原先调用系统 libcrypt 的 crypt_r()。Ubuntu 16.04 (glibc 2.23)
//           的 libcrypt 不支持任何 bcrypt 前缀（$2a$/$2b$/$2y$ 实测全部失败，
//           返回 "*0"），导致源码编译部署下注册/登录全链路不可用。
// 修改效果: 内置 Openwall crypt_blowfish 1.3（公共领域，src/bcrypt/ 目录），
//           不再依赖系统 libcrypt 的算法支持，任何 Linux 环境可直接编译运行。
//           对外接口 bcrypt_hash / bcrypt_verify 的签名与语义完全不变。
//           系统库 -lcrypt 链接保留但不再调用（无副作用）。
// 说明: crypt_blowfish 与 glibc/Openwall 参考实现输出逐字节一致；
//       _crypt_blowfish_rn 首次调用自带 quick self-test，失败返回 "*0"。
//       旧代码（crypt_r 路径）保留在文件尾部备查注释中。
// ============================================================================

#include "bcrypt_wrapper.h"

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

// 内置 crypt_blowfish 接口（C 实现，src/bcrypt/ 目录）
extern "C" {
char *_crypt_blowfish_rn(const char *key, const char *setting,
                         char *output, int size);
char *_crypt_gensalt_blowfish_rn(const char *prefix, unsigned long count,
                                 const char *input, int size,
                                 char *output, int output_size);
}

// bcrypt 输出哈希固定长度：60 字符 + 余量
#define BCRYPT_HASH_BUF_SIZE  64
// 盐值 "$2b$12$" + 22 字符 = 30 字符 + 余量
#define BCRYPT_SALT_BUF_SIZE  40

// [保留] 常量时间字符串比较，防止时序攻击
// 逐字节异或累加，比较耗时与内容无关
static bool constant_time_equals(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); i++) {
        diff |= (unsigned char)(a[i] ^ b[i]);
    }
    return diff == 0;
}

// [保留] 从 /dev/urandom 读取随机字节，交由 _crypt_gensalt_blowfish_rn
// 生成 "$2b$<cost>$<22字符>" 盐值（内部 BF_encode 消费 16 字节原始熵，
// 对应 22 字符盐 = 128 bit，与参考实现等价，无需自行做 base64 映射）
static std::string generate_bcrypt_salt(int cost)
{
    unsigned char rnd[22] = {0};
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        return "";
    }
    ssize_t got = read(fd, rnd, sizeof(rnd));
    close(fd);
    if (got != (ssize_t)sizeof(rnd)) {
        return "";
    }

    char salt[BCRYPT_SALT_BUF_SIZE] = {0};
    char *p = _crypt_gensalt_blowfish_rn("$2b$", (unsigned long)cost,
                                         (const char *)rnd, (int)sizeof(rnd),
                                         salt, (int)sizeof(salt));
    if (p == NULL || p[0] == '*') {
        return "";
    }
    return std::string(salt);
}

std::string bcrypt_hash(const std::string& password)
{
    if (password.empty()) return "";

    std::string salt = generate_bcrypt_salt(BCRYPT_COST);
    if (salt.empty()) return "";

    char output[BCRYPT_HASH_BUF_SIZE];
    memset(output, 0, sizeof(output));

    // _crypt_blowfish_rn 线程安全（输出缓冲由调用方提供，无静态状态）
    char* result = _crypt_blowfish_rn(password.c_str(), salt.c_str(),
                                      output, (int)sizeof(output));
    // 失败时返回指向 "*0" 的串（与 glibc crypt_r 失败语义一致）：
    // 结果以 '*' 开头一律视为失败，返回空串由调用方走注册失败路径。
    if (result == NULL || result[0] == '\0' || result[0] == '*') {
        return "";
    }
    return std::string(result);
}

bool bcrypt_verify(const std::string& password, const std::string& hash)
{
    // bcrypt 哈希固定 60 字符，过短必为非法格式
    if (hash.empty() || hash.size() < 4) return false;

    char output[BCRYPT_HASH_BUF_SIZE];
    memset(output, 0, sizeof(output));

    // 从 hash 中解析算法、cost、盐值并重新计算
    char* result = _crypt_blowfish_rn(password.c_str(), hash.c_str(),
                                      output, (int)sizeof(output));
    // 失败（"*0"）直接判失败；若库中 hash 恰为 "*0"（历史脏数据），
    // 重算结果同为 "*0"，常量时间比较可能误通过 —— 以 '*' 开头先行拦截。
    if (result == NULL || result[0] == '*') return false;

    return constant_time_equals(std::string(result), hash);
}

/* ============================================================================
 * 【原实现备查：系统 libcrypt crypt_r 路径】
 * Ubuntu 16.04 glibc 2.23 的 libcrypt 不支持 bcrypt（$2a/$2b/$2y 全失败返回
 * "*0"），故整体切换为内置 crypt_blowfish。原代码保留如下：
 *
 * #define _GNU_SOURCE
 * #include <crypt.h>
 *
 * std::string bcrypt_hash(const std::string& password)
 * {
 *     ...
 *     struct crypt_data data;
 *     memset(&data, 0, sizeof(data));
 *     char* result = crypt_r(password.c_str(), salt.c_str(), &data);
 *     if (result == NULL || result[0] == '\0' || result[0] == '*') return "";
 *     return std::string(result);
 * }
 *
 * bool bcrypt_verify(const std::string& password, const std::string& hash)
 * {
 *     ...
 *     struct crypt_data data;
 *     memset(&data, 0, sizeof(data));
 *     char* result = crypt_r(password.c_str(), hash.c_str(), &data);
 *     if (result == NULL || result[0] == '*') return false;
 *     return constant_time_equals(std::string(result), hash);
 * }
 * ==========================================================================*/
