#define _GNU_SOURCE  // [P0新增] 启用 GNU 扩展，使 crypt_r() 声明可见
#include "bcrypt_wrapper.h"

#include <crypt.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

// [P0新增] bcrypt 使用的变体 base64 字符集（非标准 base64）
static const char k_b64_chars[] =
    "./ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

// [P0新增] 常量时间字符串比较，防止时序攻击
// 原方案(P0文档)使用 strcmp：strcmp 遇首个差异字节即返回，可被时序侧信道探测
// 本方案：逐字节异或累加，比较耗时与内容无关
static bool constant_time_equals(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); i++) {
        diff |= (unsigned char)(a[i] ^ b[i]);
    }
    return diff == 0;
}

// [P0新增] 生成随机 bcrypt 盐值：$2b$<cost>$<22字符>
// 原方案(P0文档)使用 rand()+time()：rand() 可预测、time() 可猜测，盐值强度不足
// 本方案：从 /dev/urandom 读取 22 字节，取 byte % 64 映射到变体 base64 字符
//         因 256 = 4*64，byte % 64 无取模偏置，每个字符均匀分布
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

    char salt[32] = {0};
    snprintf(salt, sizeof(salt), "$2b$%02d$", cost);  // 生成 "$2b$12$"
    size_t prefix_len = strlen(salt);
    for (size_t i = 0; i < sizeof(rnd); i++) {
        salt[prefix_len + i] = k_b64_chars[rnd[i] % 64];
    }
    salt[prefix_len + sizeof(rnd)] = '\0';
    return std::string(salt);
}

std::string bcrypt_hash(const std::string& password)
{
    if (password.empty()) return "";

    std::string salt = generate_bcrypt_salt(BCRYPT_COST);
    if (salt.empty()) return "";

    // crypt_r 为线程安全版本；crypt() 内部使用静态缓冲，多线程不安全
    struct crypt_data data;
    memset(&data, 0, sizeof(data));

    char* result = crypt_r(password.c_str(), salt.c_str(), &data);
    //【安全修复-P0：crypt_r 失败返回 "*0"】
    // 修改原因: glibc crypt_r 失败时不返回 NULL，而是返回指向 "*0" 的字符串
    //           （并置 errno）。原代码只判 NULL/空串，会把 "*0" 当合法哈希
    //           INSERT 进库，该账号从此永远无法登录。
    // 修改效果: 结果以 '*' 开头一律视为失败，返回空串由调用方走注册失败路径。
    // 原代码: if (result == NULL || result[0] == '\0')
    if (result == NULL || result[0] == '\0' || result[0] == '*') {
        return "";
    }
    return std::string(result);
}

bool bcrypt_verify(const std::string& password, const std::string& hash)
{
    // bcrypt 哈希固定 60 字符，过短必为非法格式
    if (hash.empty() || hash.size() < 4) return false;

    struct crypt_data data;
    memset(&data, 0, sizeof(data));

    // crypt_r 会从 hash 中解析算法、cost、盐值并重新计算
    char* result = crypt_r(password.c_str(), hash.c_str(), &data);
    //【安全修复-P0：crypt_r 失败返回 "*0" 造成认证绕过】
    // 修改原因: 若库中 hash 恰为 "*0"（历史脏数据），重算结果同为 "*0"，
    //           常量时间比较返回 true —— 任意口令均可通过验证。
    // 修改效果: 结果以 '*' 开头直接判失败。
    // 原代码: if (result == NULL) return false;
    if (result == NULL || result[0] == '*') return false;

    return constant_time_equals(std::string(result), hash);
}
