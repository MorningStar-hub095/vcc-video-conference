#ifndef BCRYPT_WRAPPER_H
#define BCRYPT_WRAPPER_H

#include <string>

// [P0新增] bcrypt 密码哈希封装模块
// 基于 Linux 系统 crypt_r() 函数实现 bcrypt 自适应哈希
// 替代原有「明文 MD5 哈希直接入库」方案，大幅提升抗彩虹表/暴力破解能力
// 依赖：Linux libcrypt（编译/链接时需 -lcrypt）
// 哈希格式：$2b$12$<22字节盐><31字节哈希>，共 60 字符

// [P0新增] bcrypt 成本因子（2^12 = 4096 轮迭代，平衡安全性与登录耗时）
#define BCRYPT_COST 12

// 生成 bcrypt 哈希
// 参数：password - 待哈希的密码（客户端发来的 MD5 哈希值）
// 返回：bcrypt 哈希字符串（格式 $2b$12$...，长度 60）；失败返回空字符串
std::string bcrypt_hash(const std::string& password);

// 验证密码是否匹配 bcrypt 哈希（常量时间比较，防时序攻击）
// 参数：password - 待验证密码（客户端发来的 MD5 哈希值）
//       hash     - 数据库存储的 bcrypt 哈希字符串
// 返回：true=匹配，false=不匹配或参数错误
bool bcrypt_verify(const std::string& password, const std::string& hash);

#endif // BCRYPT_WRAPPER_H
