#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
VCC 服务端协议级冒烟测试
按 packdef.h 的自定义协议直接组包，验证「注册 -> 登录」全链路，
比只探测端口可靠得多：能同时验证 进程存活 / epoll 接收 / MySQL 读写 / bcrypt 哈希。

用法：
    # 直连明文端口（本机 Docker / 内网）
    python3 verify.py --host 127.0.0.1 --port 8000

    # 经 TLS 网关（mTLS，带客户端证书）
    python3 verify.py --host 127.0.0.1 --port 8443 --tls \
        --ca certs/ca.crt --cert certs/client.crt --key certs/client.key

协议格式（小端）：每个包 = [int32 包体长度][包体]，长度字段**不含自身 4 字节**。
    （见 block_epoll_net.cpp：recv_task 先 read 4 字节得到 nPackSize，
      再 recv nPackSize 字节作为包体；SendData 侧 nPackSize = nlen + 4，写入值为 nlen）
    包体 = PackType(int32) + 结构体剩余字段
    注册请求 STRU_REGISTER_RQ : int32 type + char[60] tel + char[60] name
                                + char[60] password + char[60] email   = 244 B
    注册回复 STRU_REGISTER_RS : int32 type + int32 result               = 8 B
    登录请求 STRU_LOGIN_RQ    : int32 type + char[40] tel + char[40] pw = 84 B
    登录回复 STRU_LOGIN_RS    : int32 type + int32 userid + int32 result
                                + char[60] name                        = 72 B
结果码：
    注册 0=手机号已存在 1=成功 2=昵称已存在
    登录 0=用户不存在 1=密码错误 2=成功 3=成功但邮箱为空
"""
import argparse, hashlib, os, socket, ssl, struct, sys, time

BASE = 10000
REGISTER_RQ, REGISTER_RS = BASE + 0, BASE + 1
LOGIN_RQ, LOGIN_RS = BASE + 2, BASE + 3
MAX_SIZE, _MAX_SIZE = 60, 40


def pkt(body: bytes) -> bytes:
    """加 4 字节长度前缀（值 = 包体长度，不含前缀本身）。"""
    return struct.pack("<i", len(body)) + body


def cstr(s: str, n: int) -> bytes:
    b = s.encode("utf-8")[: n - 1]
    return b + b"\x00" * (n - len(b))


def recv_exact(sock, n, timeout=8.0):
    sock.settimeout(timeout)
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            break
        buf += chunk
    return buf


def recv_packet(sock, timeout=8.0) -> bytes:
    """先读 4 字节长度，再读该长度的包体；返回包体（不含长度前缀）。"""
    hdr = recv_exact(sock, 4, timeout)
    if len(hdr) < 4:
        return b""
    (n,) = struct.unpack("<i", hdr)
    if n <= 0 or n > 65536:
        raise ValueError("非法包体长度: %d（协议错位？）" % n)
    return recv_exact(sock, n, timeout)


def build_ctx(ca=None, cert=None, key=None):
    """构造 TLS 上下文（可选带上客户端证书做 mTLS）。"""
    ctx = ssl.create_default_context(ssl.Purpose.SERVER_AUTH, cafile=ca)
    # 服务端证书常用 IP 作为 CN，主机名校验意义不大，这里关掉但仍强制校验证书链
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_REQUIRED
    if cert:
        ctx.load_cert_chain(cert, key or cert)
    return ctx


def make_conn(args, use_client_cert=True):
    """建立连接；args.tls 为真时先做 TLS 握手。"""
    sock = socket.create_connection((args.host, args.port), timeout=5)
    if not args.tls:
        return sock
    ctx = build_ctx(args.ca,
                    args.cert if use_client_cert else None,
                    args.key if use_client_cert else None)
    return ctx.wrap_socket(sock, server_hostname=args.host)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8000)
    ap.add_argument("--tls", action="store_true", help="经 TLS 网关连接")
    ap.add_argument("--ca", default="certs/ca.crt", help="CA 证书（校验服务端）")
    ap.add_argument("--cert", default="certs/client.crt", help="客户端证书（mTLS）")
    ap.add_argument("--key", default="certs/client.key", help="客户端私钥（mTLS）")
    ap.add_argument("--no-mtls", action="store_true",
                    help="不带客户端证书连接（用于验证网关是否真在鉴权）")
    args = ap.parse_args()

    if args.tls:
        for f in (args.ca, args.cert, args.key):
            if not os.path.isfile(f):
                print("✗ 证书文件不存在: %s（先跑 ./tls/gen-certs.sh）" % f)
                return 1

    # 服务端按「全数字且长度 11」判定为手机号（clogic.cpp LoginRq），
    # 否则会按昵称/邮箱查库导致查不到用户，因此这里必须生成 11 位号码。
    tel = "139%08d" % (int(time.time()) % 100000000)
    name = "smoke_%d" % (int(time.time()) % 100000)
    email = "%s@example.com" % name
    # 客户端传输的是 MD5(明文)，服务端再做 bcrypt
    pw = hashlib.md5(b"Smoke@12345").hexdigest()

    ok = True

    steps = 4 if (args.tls and args.cert) else 3

    # ---------- 1. 连通（TLS 场景下先握手）----------
    try:
        s = make_conn(args)
        if args.tls:
            print("✓ [1/%d] TLS 握手成功 %s:%d (%s)"
                  % (steps, args.host, args.port, s.version()))
        else:
            print("✓ [1/%d] TCP 连通 %s:%d" % (steps, args.host, args.port))
    except Exception as e:
        print("✗ [1/%d] 连接失败: %s" % (steps, e))
        return 1

    # ---------- 2. 注册 ----------
    try:
        s.sendall(pkt(struct.pack("<i", REGISTER_RQ)
                      + cstr(tel, MAX_SIZE) + cstr(name, MAX_SIZE)
                      + cstr(pw, MAX_SIZE) + cstr(email, MAX_SIZE)))
        rs = recv_packet(s)
        if len(rs) < 8:
            print("✗ [2/%d] 注册无响应（服务端未处理或崩溃）" % steps)
            return 1
        t, result = struct.unpack("<ii", rs)
        if t != REGISTER_RS:
            print("✗ [2/%d] 注册回包类型异常: %d" % (steps, t))
            return 1
        if result == 1:
            print("✓ [2/%d] 注册成功 (tel=%s)" % (steps, tel))
        else:
            print("✗ [2/%d] 注册失败 result=%d (0=手机号已存在, 2=昵称已存在)"
                  % (steps, result))
            ok = False
    except Exception as e:
        print("✗ [2/%d] 注册异常: %s" % (steps, e))
        return 1
    finally:
        s.close()

    # ---------- 3. 登录 ----------
    try:
        s = make_conn(args)
        s.sendall(pkt(struct.pack("<i", LOGIN_RQ)
                      + cstr(tel, _MAX_SIZE) + cstr(pw, _MAX_SIZE)))
        rs = recv_packet(s)
        if len(rs) < 12:
            print("✗ [3/%d] 登录无响应" % steps)
            return 1
        t, userid, result = struct.unpack("<iii", rs[:12])
        if t != LOGIN_RS:
            print("✗ [3/%d] 登录回包类型异常: %d" % (steps, t))
            return 1
        if result in (2, 3):
            print("✓ [3/%d] 登录成功 (userid=%d, result=%d)" % (steps, userid, result))
        else:
            print("✗ [3/%d] 登录失败 result=%d (0=用户不存在, 1=密码错误)"
                  % (steps, result))
            ok = False
    except Exception as e:
        print("✗ [3/%d] 登录异常: %s" % (steps, e))
        return 1
    finally:
        s.close()

    # ---------- 4. 反向鉴权：无客户端证书必须被拒绝 ----------
    if steps == 4:
        # 【修复-P1：bad 未初始化导致 NameError】
        # 修改原因: 原代码在 try 块内才给 bad 赋值；若 make_conn() 握手即抛异常
        #           （这正是"无证书被拒绝"的正常场景），finally 里 bad.close()
        #           因 bad 未定义抛 NameError，脚本以 traceback 非 0 退出，
        #           调用方把"鉴权正确拒绝"误判为验证失败。
        # 修改效果: 先置 bad = None，finally 判空再 close；
        #           "握手即被拒"恢复为预期的 ✓ 分支，退出码正确。
        # 原代码: （try 内首次赋值，finally 直接 bad.close()）
        bad = None
        try:
            bad = make_conn(args, use_client_cert=False)
            bad.sendall(pkt(struct.pack("<i", LOGIN_RQ)
                            + cstr(tel, _MAX_SIZE) + cstr(pw, _MAX_SIZE)))
            # 能收到回包 = 网关没做鉴权，属于安全缺陷
            bad.settimeout(5)
            if bad.recv(16):
                print("✗ [4/4] 鉴权失效：无客户端证书也能通过网关！")
                ok = False
            else:
                print("✓ [4/4] 无证书连接被网关拒绝（mTLS 鉴权生效）")
        except Exception as e:
            print("✓ [4/4] 无证书连接被网关拒绝（mTLS 鉴权生效）: %s"
                  % type(e).__name__)
        finally:
            try:
                bad.close()
            except Exception:
                pass

    print("\n" + ("✓ 全部通过：服务端进程 / epoll / MySQL / bcrypt 链路正常"
                  + ("；TLS 加密与 mTLS 鉴权生效" if args.tls else "")
                  if ok else "✗ 存在失败项，请查看服务端日志"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
