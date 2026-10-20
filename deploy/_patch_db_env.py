# -*- coding: utf-8 -*-
"""
P0-2 补丁：把 packdef.h 里硬编码的数据库账号/口令改为运行期读取环境变量。
对「非权威副本」同样打补丁，确保明文口令在整个项目里彻底消失。

用法： python3 _patch_db_env.py
幂等：已打过补丁的文件会跳过（检测到 _db_env 即跳过）。
"""
import io
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

# 需要打补丁的 packdef.h（权威源在前）
TARGETS = [
    os.path.join(ROOT, "服务端代码", "PlayerHallServer", "include", "packdef.h"),
    os.path.join(ROOT, "服务端代码", "PlayerHallServer", "PlayerHallServer",
                 "include", "packdef.h"),
    os.path.join(ROOT, "服务端代码", "PlayerHallServer", "PlayerHallServer1",
                 "PlayerHallServer", "include", "packdef.h"),
]

# 注意：旧口令在此脱敏为占位符（该文件会被 Git 跟踪，绝不能再写回真实口令）。
# 本补丁已对仓库内全部 packdef.h 生效且幂等，历史明文口令已作废，
# 如需在新环境复现，把 <REDACTED_OLD_DB_PWD> 换成当时实际的旧口令即可。
OLD_BLOCK = '''/*-------------数据库信息-----------------*/
#define _DEF_DB_NAME    "VCC"
#define _DEF_DB_IP      "localhost"
#define _DEF_DB_USER    "root"
#define _DEF_DB_PWD     "<REDACTED_OLD_DB_PWD>"
'''

NEW_BLOCK = '''/*-------------数据库信息-----------------*/
// [安全修复 P0-2] 数据库账号口令不再硬编码，改为运行期读取环境变量。
// 修改原因: 原实现把 root 口令以明文 #define 写死在头文件里，随源码同步扩散，
//           任何拿到代码的人都能直连数据库。
// 修改内容: 新增 _db_env() 助手：环境变量存在且非空则用它，否则回退到 def。
// 影响范围: _DEF_DB_NAME / _DEF_DB_IP / _DEF_DB_USER / _DEF_DB_PWD 四个宏，
//           使用方（Mysql.cpp 的 mysql_real_connect）无需改动。
// 注意事项: 部署时必须注入 DB_NAME / DB_IP / DB_USER / DB_PWD 四个环境变量，
//           否则回退到下面的默认值（口令默认为空，将连不上受口令保护的库）。
static inline const char* _db_env(const char* key, const char* def){
    const char* v = getenv(key);
    return (v && *v) ? v : def;
}
#define _DEF_DB_NAME    _db_env("DB_NAME", "vcc")
#define _DEF_DB_IP      _db_env("DB_IP",   "localhost")
#define _DEF_DB_USER    _db_env("DB_USER", "vcc")
#define _DEF_DB_PWD     _db_env("DB_PWD",  "")
'''


def main() -> int:
    changed = 0
    for path in TARGETS:
        if not os.path.isfile(path):
            print("  - 跳过（不存在）: %s" % os.path.relpath(path, ROOT))
            continue
        with io.open(path, "r", encoding="utf-8", errors="surrogateescape") as f:
            text = f.read()
        if "_db_env" in text:
            print("  = 已打过补丁，跳过: %s" % os.path.relpath(path, ROOT))
            continue
        if OLD_BLOCK not in text:
            print("  ! 未匹配到原始块，请人工检查: %s" % os.path.relpath(path, ROOT))
            continue
        text = text.replace(OLD_BLOCK, NEW_BLOCK)
        with io.open(path, "w", encoding="utf-8", errors="surrogateescape",
                     newline="") as f:
            f.write(text)
        changed += 1
        print("  ✓ 已打补丁: %s" % os.path.relpath(path, ROOT))
    print("\n共修改 %d 个文件" % changed)
    return 0


if __name__ == "__main__":
    sys.exit(main())
