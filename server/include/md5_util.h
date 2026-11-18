#ifndef MD5_UTIL_H
#define MD5_UTIL_H

#include <string>

/*【2026-11-18 审查修复-C1：登录通道 challenge-response】服务端标准 MD5 计算工具
 *
 * 历史: 初版（文件尾部 #if 0 注释保留）复刻客户端 vcc-client/md5/md5.cpp 并做
 *       64 位平台适配，经 RFC 1321 标准向量验证【输出错误】——随后发现客户端的
 *       MD5 类本身也不是标准 MD5（"abc" 应为 900150983cd24fb0d6963f7d28e17f72，
 *       客户端实现输出 7999dc75...；空串/64 字符向量同样不符）。两端此前从未
 *       独立验证过，只因"客户端算的存进库、服务端只做比较"而从未暴露。
 * 现状: 本文件改为【RFC 1321 标准实现】，仅用于 challenge 应答
 *       resp = MD5( ASCII(h_hex + nonce_hex) ) 的计算；两端必须都用标准 MD5
 *       （客户端侧用 Qt 自带 QCryptographicHash::Md5，同为标准实现）。
 * 注意: 客户端 GetMD5(口令) 产出的 h（存量账号的口令等价物）保持原实现不变，
 *       不受本文件影响；其非标准性已在审查报告与工作日志中记录，属遗留问题。
 *
 * 接口: md5_hex(输入字节串) → 32 字符小写十六进制（标准 MD5）。
 */
std::string md5_hex(const std::string& src);

#endif // MD5_UTIL_H
