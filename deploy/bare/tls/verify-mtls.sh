#!/usr/bin/env bash
# ============================================================
# mTLS 改造验收脚本（裸机 / 虚拟机上直接跑）
#
# 作用：把「正常证书握手成功」「已吊销证书被拒绝」两件事做成可重复执行的断言，
#       并顺带核对 CRL 内容与 CRL 分发点是否可访问。
#
# 用法：
#   sudo ./verify-mtls.sh \
#        --host meet.example.com --port 8443 \
#        --good    /opt/vcc-ca/intermediate/users/u001 \
#        --revoked /opt/vcc-ca/intermediate/users/u002
#
# 参数：
#   --host/-p    网关地址与端口（默认 127.0.0.1:8443）
#   --good       正常用户目录（内含 user.crt / user.key）
#   --revoked    已吊销用户目录（内含 user.crt / user.key）
#   --ca         校验服务端证书用的信任库，默认系统信任库
#                （服务端证书已改公共 CA，所以默认即可）
#   --crl-url    CRL 分发点，默认从 /opt/vcc-ca/crl-url.txt 读
#   --log        nginx stream 访问日志，默认 /var/log/nginx/vcc-stream.log
#
# 退出码： 0 = 三项断言全过； 非 0 = 有断言失败（会打印哪一项没过）
# ============================================================
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

CA_DIR="${CA_DIR:-/opt/vcc-ca}"
TLS_DIR="${TLS_DIR:-/etc/vcc-tls}"
HOST="127.0.0.1"; PORT=8443
GOOD=""; REVOKED=""; CAFILE=""; CRL_URL=""; LOG="/var/log/nginx/vcc-stream.log"
NOCHECK=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --host)    HOST="${2:-}";    shift 2 ;;
    --port|-p) PORT="${2:-}";    shift 2 ;;
    --good)    GOOD="${2:-}";    shift 2 ;;
    --revoked) REVOKED="${2:-}"; shift 2 ;;
    --ca)      CAFILE="${2:-}";  shift 2 ;;
    --crl-url) CRL_URL="${2:-}"; shift 2 ;;
    --log)     LOG="${2:-}";     shift 2 ;;
    --no-check-hostname) NOCHECK="nocheck"; shift ;;
    -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "未知参数: $1" >&2; exit 2 ;;
  esac
done

[[ -n "$GOOD" && -n "$REVOKED" ]] || { echo "请给 --good 与 --revoked（用户证书目录）" >&2; exit 2; }
[[ -f "$GOOD/user.crt"    && -f "$GOOD/user.key" ]]    || { echo "缺少 $GOOD/user.{crt,key}" >&2; exit 2; }
[[ -f "$REVOKED/user.crt" && -f "$REVOKED/user.key" ]] || { echo "缺少 $REVOKED/user.{crt,key}" >&2; exit 2; }

CAFILE="${CAFILE:-/etc/ssl/certs/ca-certificates.crt}"
[[ -f "$CAFILE" ]] || CAFILE="$(python3 -c 'import ssl;print(ssl.get_default_verify_paths().openssl_cafile)' 2>/dev/null)"
CRL_URL="${CRL_URL:-$(cat "$CA_DIR/crl-url.txt" 2>/dev/null)}"

# 单次连接探测：握手 + 发 8 字节 + 尝试读回
# 注意：TLS1.3 下客户端会在收到服务端 alert 之前就认为握手完成，
#       所以「握手成功」不能当作放行依据，必须以「能否读写业务数据」为准。
probe() {
  python3 - "$HOST" "$PORT" "$CAFILE" "$1/user.crt" "$1/user.key" "$NOCHECK" <<'PY'
import socket, ssl, sys
host, port, cafile, cert, key = sys.argv[1:6]
NOCHECK = (len(sys.argv) > 6 and sys.argv[6] == "nocheck")

ctx = ssl.create_default_context(cafile=cafile)
ctx.load_cert_chain(cert, key)
if NOCHECK:                      # 用 IP 直连、证书里只有域名 SAN 时打开
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_REQUIRED

try:
    s = ctx.wrap_socket(socket.create_connection((host, int(port)), 5),
                        server_hostname=host)
    ver = s.version()
except ssl.SSLError as e:
    print("握手阶段即被拒: SSLError(%s)" % e.reason); sys.exit(4)
except Exception as e:
    print("连接失败: %r" % e); sys.exit(5)

try:
    s.settimeout(5)
    s.sendall(b"\x00\x00\x00\x04PING")     # 一个最小业务包（4 字节长度前缀 + 4 字节体）
    data = s.recv(64)
except socket.timeout:
    # 连上了、包也发出去了，服务端只是不认识 PING 没回包 —— 说明链路是通的
    print("TLS=%s 握手成功，连接保持（未被网关切断）" % ver); s.close(); sys.exit(0)
except ssl.SSLError as e:
    print("TLS=%s 握手后收到 TLS 告警: %s" % (ver, e.reason)); sys.exit(3)
except (ConnectionResetError, BrokenPipeError):
    print("TLS=%s 连接被网关重置" % ver); sys.exit(3)

if data == b"":
    print("TLS=%s 握手后即刻被服务端关闭（未连到后端）" % ver); sys.exit(3)
print("TLS=%s 握手成功并收到 %d 字节回包" % (ver, len(data))); s.close(); sys.exit(0)
PY
}

FAIL=0

echo "===== 1/4 正常证书（$GOOD）====="
if OUT="$(probe "$GOOD")"; then echo "  ✓ 握手并收发数据成功： $OUT"; else
  echo "  ✗ 正常证书居然不通： $OUT"; FAIL=1; fi

echo
echo "===== 2/4 已吊销证书（$REVOKED）====="
ROUT="$(probe "$REVOKED")"; RRC=$?
RSERIAL="$(openssl x509 -in "$REVOKED/user.crt" -noout -serial | sed 's/serial=//')"
if [[ $RRC -ne 0 ]]; then echo "  ✓ 被网关拒绝： $ROUT（exit=$RRC）"; else
  echo "  ✗ 已吊销证书竟然建立了可用连接： $ROUT"; FAIL=1; fi

echo
echo "===== 3/4 CRL 内容 ====="
CRLFILE="$TLS_DIR/client-ca/crl.pem"
if [[ -f "$CRLFILE" ]]; then
  echo "  文件     : $CRLFILE"
  echo "  签发者   : $(openssl crl -in "$CRLFILE" -noout -issuer 2>/dev/null | sed 's/issuer=//')"
  echo "  下次更新 : $(openssl crl -in "$CRLFILE" -noout -nextupdate 2>/dev/null | sed 's/nextUpdate=//')"
  echo "  吊销条目 : $(openssl crl -in "$CRLFILE" -noout -text 2>/dev/null | grep -c 'Serial Number:')"
  if openssl crl -in "$CRLFILE" -noout -text 2>/dev/null | grep -q "Serial Number: $RSERIAL$"; then
    echo "  ✓ 已吊销证书序列号 $RSERIAL 在 CRL 中"
  else
    echo "  ✗ 序列号 $RSERIAL 不在 CRL 中"; FAIL=1
  fi
else
  echo "  ✗ CRL 文件不存在：$CRLFILE"; FAIL=1
fi

echo
echo "===== 4/4 网关日志与 CRL 分发点 ====="
if [[ -r "$LOG" ]]; then
  echo "  -- $LOG 最近 3 条 --"
  grep "verify=" "$LOG" 2>/dev/null | tail -3 | sed 's/^/     /'
  if grep -q "verify=FAILED" "$LOG" 2>/dev/null; then
    echo "  ✓ 网关侧明确判定 FAILED（$ssl_client_verify）"
    grep "verify=FAILED" "$LOG" | tail -1 | grep -o 'verify=[^ ]*' | sed 's/^/     判定: /'
  else
    echo "  ⚠ 日志中未出现 verify=FAILED（若刚吊销，确认已 reload 网关）"
  fi
else
  echo "  ⚠ 无法读取网关日志 $LOG（非 root 或路径不同），跳过"
fi
if [[ -n "$CRL_URL" ]]; then
  if command -v curl >/dev/null 2>&1; then
    CODE="$(curl -s -o /tmp/_crl.pem -w '%{http_code}' "$CRL_URL" 2>/dev/null)"
    SZ="$(wc -c </tmp/_crl.pem 2>/dev/null | tr -d ' ')"
    if [[ "$CODE" == "200" && "${SZ:-0}" -gt 0 ]]; then
      echo "  ✓ CRL 分发点可访问：$CRL_URL （HTTP $CODE, ${SZ} bytes）"
    else
      echo "  ✗ CRL 分发点不可访问：$CRL_URL （HTTP ${CODE:-000}）"; FAIL=1
    fi
  fi
fi

echo
echo "================= 验收结论 ================="
if [[ $FAIL -eq 0 ]]; then
  echo "✓ 全部通过：正常证书可通信 / 已吊销证书被拒 / CRL 已发布"
else
  echo "✗ 存在未通过项，见上面标 ✗ 的行"
fi
exit "$FAIL"
