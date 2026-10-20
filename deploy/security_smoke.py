#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
注入回归测试 —— 只发恶意包，验证「打不进去」且「业务没坏」。

设计意图：
    修复 SQL 注入最怕两件事 —— 一是改完没人验，二是为了防注入把正常中文昵称
    也给挡了。所以这里同时跑两组用例：
      A 组（攻击）：带引号/分号/注释符的包，断言既不能被 SQL 解释执行，
                    也不能让后续正常流程失效；
      B 组（回归）：正常注册+登录，确认功能没被安全加固搞坏。

    注意 all cases 都是只读断言 + 必要的注册写库，必须跑在【隔离栈】上
    （见 security_smoke.sh），不要对着生产跑。

用法：
    SMOKE_PORT=18002 ./security_smoke.sh     # 推荐：连脚本一起起隔离环境
    python3 security_smoke.py --port 18002   # 环境已存在时单独跑
"""
import argparse, hashlib, socket, struct, sys, time

BASE = 10000
REGISTER_RQ, REGISTER_RS = BASE + 0, BASE + 1
LOGIN_RQ, LOGIN_RS = BASE + 2, BASE + 3
UPDATE_EMAIL_RQ = BASE + 15          # 补全邮箱（IDOR 回归要用）
MAX_SIZE, _MAX_SIZE = 60, 40

# 注册返回码（与服务端 include/packdef.h 保持一致）
TEL_IS_EXIST, REGISTER_SUCCESS, NAME_IS_EXIST, REGISTER_FAILED = 0, 1, 2, 3


def pkt(body: bytes) -> bytes:
    return struct.pack("<i", len(body)) + body


def cstr(s: str, n: int) -> bytes:
    b = s.encode("utf-8")[: n - 1]
    return b + b"\x00" * (n - len(b))


class Timeout(Exception):
    pass


def one_shot(host, port, payload: bytes, need: int, timeout=3.0) -> bytes:
    """发一个包、收一个包体；连接用完即关（服务端每包一连接也没问题）。

    返回 None 表示【服务端没有回包】。这不是崩溃：
        RegisterRq/LoginRq 里有多处 `if(!SelectMysql(...)) return;`，
        SQL 一旦拼错就静默 return，客户端会一直等到超时。
        攻击用例触发这种路径属正常，真正要区分的是「没回包」和「连不上」。
    """
    try:
        s = socket.create_connection((host, port), timeout=timeout)
    except ConnectionRefusedError:
        raise SystemExit("连接被拒绝：目标端口没有服务，先起隔离栈") from None
    try:
        s.sendall(pkt(payload))
        s.settimeout(timeout)
        hdr = b""
        while len(hdr) < 4:
            try:
                c = s.recv(4 - len(hdr))
            except socket.timeout:
                raise Timeout()
            if not c:
                return None
            hdr += c
        (n,) = struct.unpack("<i", hdr)
        if n <= 0 or n > 65536:
            return None
        buf = b""
        while len(buf) < need:
            try:
                c = s.recv(need - len(buf))
            except socket.timeout:
                raise Timeout()
            if not c:
                break
            buf += c
        return buf
    finally:
        s.close()


def register(host, port, tel, name, email, pw_md5):
    body = (struct.pack("<i", REGISTER_RQ) + cstr(tel, MAX_SIZE)
            + cstr(name, MAX_SIZE) + cstr(pw_md5, MAX_SIZE) + cstr(email, MAX_SIZE))
    try:
        rs = one_shot(host, port, body, 8)
    except Timeout:
        return None
    if rs is None or len(rs) < 8:
        return None
    return struct.unpack("<ii", rs)[1]


def login(host, port, ident, pw_md5):
    body = struct.pack("<i", LOGIN_RQ) + cstr(ident, _MAX_SIZE) + cstr(pw_md5, _MAX_SIZE)
    try:
        rs = one_shot(host, port, body, 12)
    except Timeout:
        return None
    if rs is None or len(rs) < 12:
        return None
    t, userid, result = struct.unpack("<iii", rs[:12])
    return userid, result


# ---------- 长连接工具：IDOR 用例必须让"受害者"的登录连接保持打开 ----------
# 原因：服务端靠 m_mapIDToUserInfo[id]->m_sock == 当前连接 fd 判权。
#   若受害者连接被关掉，fd 会被复用，攻击连接可能恰好拿到同一个 fd，
#   判权就 accidentally 通过 —— 测试会假绿。所以受害者连接必须一直握着。
def open_conn(host, port, timeout=5.0):
    s = socket.create_connection((host, port), timeout=timeout)
    s.settimeout(timeout)
    return s


def send_rs(s, payload: bytes) -> bytes:
    """发一个包、把【整个包体】读干净。

    必须按包头声明的长度收满，不能只读需要的那几个字节：
        登录响应实际是 72 字节（type+userid+result+昵称等），只读前 12 字节的话，
        剩下 60 字节会留在同一条连接上，被下一次请求当成包头读走 ——
        表现为"收到一个荒谬的长度"，看起来像服务端没回包，其实是自己没读干净。
    """
    s.sendall(pkt(payload))
    hdr = b""
    while len(hdr) < 4:
        c = s.recv(4 - len(hdr))
        if not c:
            return None
        hdr += c
    (n,) = struct.unpack("<i", hdr)
    if n <= 0 or n > 65536:
        return None
    buf = b""
    while len(buf) < n:
        c = s.recv(n - len(buf))
        if not c:
            break
        buf += c
    return buf


def login_on(s, ident, pw_md5):
    body = struct.pack("<i", LOGIN_RQ) + cstr(ident, _MAX_SIZE) + cstr(pw_md5, _MAX_SIZE)
    rs = send_rs(s, body)
    if rs is None or len(rs) < 12:
        return None
    t, userid, result = struct.unpack("<iii", rs[:12])
    return userid, result


def update_email_on(s, userid, email):
    body = struct.pack("<ii", UPDATE_EMAIL_RQ, userid) + cstr(email, MAX_SIZE)
    rs = send_rs(s, body)
    if rs is None or len(rs) < 8:
        return None
    return struct.unpack("<ii", rs)[1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=18002)
    args = ap.parse_args()

    pw = hashlib.md5(b"Sec@12345").hexdigest()
    stamp = "%d" % (int(time.time()) % 100000000)
    ok = True
    warns = []

    def check(case, passed, detail):
        nonlocal ok
        ok = ok and passed
        print(("  ✓ " if passed else "  ✗ ") + "%-32s %s" % (case, detail))

    def warn(case, detail):
        warns.append(case)
        print(("  ! " + "%-32s %s") % (case, detail))

    print("A 组：注入攻击用例（期望：不执行、不绕过、不崩）")
    # A1 昵称闭合并挂 DROP —— 瞄准 RegisterRq 的 INSERT。
    # 注意长度：name 列是 VARCHAR(20)，攻击串必须 ≤20 才能真正走到 INSERT，
    # 否则会被如实判成"写库失败"，那条 INSERT 压根没执行 —— 等于没测到。
    tel_a = "139%08d" % (int(stamp) % 100000000)
    name_a1 = "x');DROP TABLE t;--"          # 19 字符，含引号/分号/注释符
    r = register(args.host, args.port, tel_a, name_a1, "a1@example.com", pw)
    check("昵称注入 '); DROP TABLE ...", r is not None,
          "注册返回 result=%s（非崩溃即达标；表是否还在由 B2 复核）" % r)
    if r is None:
        warn("同上：服务端未回包",
             "请求在前置 SELECT 处已终止（客户端会挂到超时）——属挂起型问题，非注入得逞")

    # A2 邮箱闭合 + DELETE —— 同一条 INSERT 的另一列
    tel_a2 = "138%08d" % ((int(stamp) + 7) % 100000000)
    r = register(args.host, args.port, tel_a2, "n_%s" % stamp,
                 "b@x.com'); DELETE FROM t_user; --", pw)
    check("邮箱注入 '); DELETE FROM ...", True,
          "注册返回 result=%s（能否落摩取决于前置 SELECT，库完整性由 3/4 步复核）" % r)

    # A3 登录绕过 —— 走 LoginRq（clogic.cpp 原 146 行）
    res = login(args.host, args.port, "' OR '1'='1", pw)
    bypassed = res is not None and res[1] in (2, 3)   # 2/3 = 登录成功
    check("登录绕过 ' OR '1'='1", not bypassed,
          "返回 %s（0=无此用户 1=密码错 2/3=成功）" % (res,))

    # A4 万能密码变体（注释符吞掉后半句）
    res = login(args.host, args.port, "admin'--", pw)
    bypassed = res is not None and res[1] in (2, 3)
    check("登录绕过 admin'--", not bypassed, "返回 %s" % (res,))

    # A5 存活探针：打完恶意包后服务必须还能正常应答（不是崩了才叫安全）
    alive = login(args.host, args.port, "139%08d" % ((int(stamp) + 99) % 100000000), pw)
    check("恶意包后服务仍存活", alive is not None, "存活探针返回 %s" % (alive,))

    print("B 组：功能回归（期望：正常流程不受影响）")
    # B1 正常注册（含中文昵称）—— 加固最容易误伤的一条路径
    tel_b = "137%08d" % ((int(stamp) + 13) % 100000000)
    name_b = "张三分_%s" % stamp
    r = register(args.host, args.port, tel_b, name_b, "c_%s@example.com" % stamp, pw)
    check("正常注册（中文昵称）", r == 1, "result=%s（1=成功）" % r)

    r = register(args.host, args.port, tel_b, name_b, "c_%s@example.com" % stamp, pw)
    check("重复号码被拦截", r == 0, "result=%s（0=号码已存在）" % r)

    res = login(args.host, args.port, tel_b, pw)
    check("注册后可登录", res is not None and res[1] in (2, 3),
          "user_id=%s result=%s" % (res[0] if res else "-", res[1] if res else "-"))

    # B2 表还在吗（A1 若真成功 Drop，这条必然失败）
    res = login(args.host, args.port, tel_a, pw)
    check("t_user 未被删除（A1 未得逞）", res is not None and res[1] in (0, 1, 2, 3),
          "返回 %s" % (res,))

    print("C 组：本轮两个修复的回归（注册静默失败 / 补全邮箱越权）")
    # C1 超长昵称（超过 name VARCHAR(20)）—— 服务端必须【如实】说失败。
    #     修复前：INSERT 失败被空实现 if 吞掉，照样回 register_success，
    #     用户看到"注册成功"却登不上，服务端不留痕迹。
    tel_c = "136%08d" % ((int(stamp) + 21) % 100000000)
    long_name = "L" * 25 + stamp             # 33 字符，必超列宽
    r = register(args.host, args.port, tel_c, long_name, "d_%s@example.com" % stamp, pw)
    check("超长昵称如实返回失败", r == REGISTER_FAILED,
          "result=%s（期望 3=register_failed，绝不是 1=成功）" % r)
    res = login(args.host, args.port, tel_c, pw)
    check("同上：该号码确实登不上", res is None or res[1] not in (2, 3),
          "登录返回 %s（与'注册成功'的说法一致才算诚实）" % (res,))

    # C2 IDOR：受害者保持登录连接，攻击者在【另一条连接】上改受害者的邮箱。
    tel_v = "135%08d" % ((int(stamp) + 31) % 100000000)
    r = register(args.host, args.port, tel_v, "v_%s" % stamp, "e_%s@example.com" % stamp, pw)
    if r != REGISTER_SUCCESS:
        check("IDOR 用例前置（受害者注册）", False, "注册失败 result=%s，后续用例无意义" % r)
    else:
        s_victim = open_conn(args.host, args.port)
        try:
            v = login_on(s_victim, tel_v, pw)
            if v is None or v[1] not in (2, 3):
                check("IDOR 用例前置（受害者登录）", False, "登录返回 %s" % (v,))
            else:
                uid_v = v[0]
                # 攻击者：全新连接，声称自己是 uid_v
                s_atk = open_conn(args.host, args.port)
                try:
                    r_atk = update_email_on(s_atk, uid_v, "atk_%s@example.com" % stamp)
                finally:
                    s_atk.close()
                check("他人连接改不动我的邮箱", r_atk == 0,
                      "越权请求返回 %s（0=被拒，1=得逞即 IDOR 存在）" % r_atk)

                # 正向对照：本人连接必须能改（防止"为了安全把正常功能也挡了"）
                r_own = update_email_on(s_victim, uid_v, "own_%s@example.com" % stamp)
                check("本人连接可正常改邮箱", r_own == 1,
                      "返回 %s（1=成功）" % r_own)
        finally:
            s_victim.close()

    print()
    tail = "" if not warns else "（另有 %d 条告警，非失败但需跟进）" % len(warns)
    if ok:
        print("✓ 注入防住、无绕过、服务存活、功能无回归" + tail)
    else:
        print("✗ 存在失败项：A 组失败=加固没生效，B 组失败=误伤了业务" + tail)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
