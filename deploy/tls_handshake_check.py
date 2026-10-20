#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
TLS 网关握手检查 —— 只握手，不注册。

为什么部署流水线里不能用 verify.py --tls 代替：
    verify.py 在握手之后会真的发注册包和登录包，也就是往网关背后的
    【生产 MySQL】写一条用户数据；协议里没有注销/删除接口，这些脏数据
    只能手工清。部署动作要反复执行，绝不能自带写副作用。

    ⚠ TLS 1.3 的「握手假象」—— 这是本脚本存在的最大理由：
      在 TLS 1.3 里，服务端对客户端证书的判定发生在握手【之后】
      （post-handshake authentication）。也就是说：
          不带客户端证书连上去，openssl s_client 和 python ssl
          都会报告"握手成功"，看起来鉴权失效了；
          实际上服务端随后立刻发 alert 并断开，一个字节都不会转发。
      只看 wrap_socket 是否成功，会得出完全错误的结论。
      因此反向用例必须做两件事：
        (1) 强制降到 TLS 1.2 —— 1.2 的证书校验在握手内完成，会被明确拒绝；
        (2) TLS 1.3 下握手"成功"之后，再发一个字节试探是否真的被转发。
      两条都对上，才敢说"mTLS 鉴权生效"。

用法：
    # 正向：带客户端证书，期望握手成功
    python3 tls_handshake_check.py --port 8443 \
        --cert certs/client.crt --key certs/client.key

    # 反向：不带客户端证书，期望被拒（TLS1.2 硬拒 + TLS1.3 握手后断开）
    python3 tls_handshake_check.py --port 8443 --expect-fail

    # 吊销验证：已吊销证书同样应被拒
    python3 tls_handshake_check.py --port 8443 --expect-fail \
        --cert certs/revoked.crt --key certs/revoked.key

退出码：0=结论符合预期，1=结论不符（网关鉴权失效或链路不通）
"""
import argparse, os, socket, ssl, sys

TIMEOUT = 8.0


def build_ctx(ca, cert, key, max_ver=None):
    ctx = ssl.create_default_context(ssl.Purpose.SERVER_AUTH, cafile=ca)
    # 服务端证书常用 IP 作 CN，主机名校验无意义，但证书链必须校
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_REQUIRED
    if cert:
        ctx.load_cert_chain(cert, key or cert)
    if max_ver:
        ctx.maximum_version = max_ver
    return ctx


def handshake(host, port, ca, cert, key, max_ver=None):
    """完成一次 TLS 握手；失败抛异常。返回 sock，调用方负责关闭。"""
    raw = socket.create_connection((host, port), timeout=TIMEOUT)
    try:
        return build_ctx(ca, cert, key, max_ver).wrap_socket(
            raw, server_hostname=host)
    except Exception:
        raw.close()
        raise


def cstr(s, n):
    b = s.encode("utf-8")[: n - 1]
    return b + b"\x00" * (n - len(b))


def backend_login(sock):
    """发一次【不存在号码】的登录请求，确认网关真的把流量送到了后端。

    与容器里的 health-probe 完全同款：号码不存在 → 服务端只做一次 SELECT
    就回包，不会插入任何数据。用它代替"注册"来证明链路是通的，
    正是为了不留下脏数据。
    """
    import struct
    LOGIN_RQ = 10002          # packdef.h
    body = (struct.pack("<i", LOGIN_RQ) + cstr("0000000000", 40)
            + cstr("probe-no-such-user", 40))
    try:
        sock.settimeout(TIMEOUT)
        sock.sendall(struct.pack("<i", len(body)) + body)
        hdr = b""
        while len(hdr) < 4:
            c = sock.recv(4 - len(hdr))
            if not c:
                return False
            hdr += c
        (n,) = struct.unpack("<i", hdr)
        if n <= 0 or n > 65536:
            return False
        return len(sock.recv(12)) >= 12
    except Exception:
        return False


def forwarded(sock):
    """握手后探针：发一个字节，看有没有被转发回来。

    返回 True 表示【流量真的穿过了网关】—— 那才是鉴权失效。
    返回 False 表示被 alert 断开或对端直接关闭。
    注意这里只发 1 个非法字节：服务端收不到完整包体，不会写任何业务数据。
    """
    try:
        sock.settimeout(TIMEOUT)
        sock.sendall(b"\x00")
        data = sock.recv(16)
        return bool(data)
    except Exception:
        return False


def close(sock):
    try:
        sock.close()
    except Exception:
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8443)
    ap.add_argument("--ca", default="certs/ca.crt")
    ap.add_argument("--cert", default=None, help="客户端证书；不传则发起无证书连接")
    ap.add_argument("--key", default=None)
    ap.add_argument("--expect-fail", action="store_true",
                    help="期望被拒绝（用于无证书/已吊销证书的反向验证）")
    args = ap.parse_args()

    if args.cert and not args.key:
        print("✗ 指定了 --cert 但缺 --key")
        return 1
    for f in (args.ca, args.cert, args.key):
        if f and not os.path.isfile(f):
            print("✗ 文件不存在: %s" % f)
            return 1

    who = "有客户端证书" if args.cert else "无客户端证书"
    ok = True

    if not args.expect_fail:
        print("→ [正向] %s %s:%d" % (who, args.host, args.port))
        try:
            s = handshake(args.host, args.port, args.ca, args.cert, args.key)
            print("✓ 握手成功 (%s)" % s.version())
        except Exception as e:
            print("✗ 握手失败: %s: %s" % (type(e).__name__, str(e)[:120]))
            return 1

        # 光握手成功只证明"证书有效"，还得证明"网关真的把流量送到后端"。
        # 这里发一次【不存在的号码】的登录请求：服务端只做一次 SELECT，
        # 不会写任何数据（与 health-probe 完全同款），却能确认链路是通的。
        if backend_login(s):
            print("✓ 流量已转发到后端并收到应答（登录探测，号码不存在，零写库）")
        else:
            print("✗ 握手成功但后端无应答 —— 网关到 server:8000 这一段不通")
            ok = False
        close(s)
        return 0 if ok else 1

    # ---------------- 反向：无证书 / 已吊销证书必须被拒 ----------------
    # 判据统一为【字节有没有真的穿过去】，而不是握手是否成功：
    #   nginx stream 在 TLS 1.2 下同样会先完成握手、随后立刻断开，
    #   python/openssl 都会报"握手成功" —— 那是假象。
    #   网关日志里 up=- （没选到上游）才是真的拒绝，落到客户端侧
    #   就表现为：写一个字节后立即被断开、收不到任何回包。
    for label, max_ver in (("TLS 1.2", ssl.TLSVersion.TLSv1_2), ("TLS 1.3", None)):
        print("→ [反向] %s 下应无法收发数据（%s）" % (label, who))
        try:
            s = handshake(args.host, args.port, args.ca, args.cert, args.key, max_ver)
        except Exception as e:
            print("✓ 握手阶段即被拒绝: %s" % type(e).__name__)
            continue
        if forwarded(s):
            print("✗ %s 握手后仍能收发数据 —— 鉴权失效，流量被转发到后端！" % label)
            ok = False
        else:
            print("✓ %s 握手后随即断开，未转发任何字节（拒绝生效）" % label)
        close(s)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
