#!/usr/bin/env bash
# ============================================================
# 吊销客户端证书，并立即重新生成 + 发布 CRL
#
# 用法：
#   sudo ./revoke-user.sh --uid u001
#   sudo ./revoke-user.sh --serial 1A2B3C
#   sudo ./revoke-user.sh --uid u001 --reason keyCompromise
#
# reason: unspecified|keyCompromise|CACompromise|cessationOfOperation
#         |superseded|certificateHold  (默认 keyCompromise)
#
# 生效：CRL 重新发布 + 网关 reload，已吊销证书在下一次握手即被拒绝。
# ============================================================
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=ca-lib.sh
source "$HERE/ca-lib.sh"

UID_=""; SERIAL=""; REASON="keyCompromise"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --uid)    UID_="${2:-}";    shift 2 ;;
    --serial) SERIAL="${2:-}";  shift 2 ;;
    --reason) REASON="${2:-}";  shift 2 ;;
    *) die "未知参数: $1" ;;
  esac
done

need_file "$ICA_DIR/openssl.cnf"
need_file "$INVENTORY"

# ---------- 1. 定位证书 ----------
if [[ -z "$SERIAL" ]]; then
  [[ -n "$UID_" ]] || die "请给 --uid 或 --serial"
  # 只挑「尚未吊销」的那条：台账里状态列形如 issued / revoked:2026-...
  SERIAL="$(awk -F, -v u="$UID_" '$2==u && index($8,"revoked")==0 {print $1}' "$INVENTORY" | tail -1)"
  [[ -n "$SERIAL" ]] || die "台账里找不到未吊销的 $UID_"
fi
SERIAL_UP="$(echo "$SERIAL" | tr 'a-f' 'A-F')"
CERT="$ICA_DIR/newcerts/${SERIAL_UP}.pem"
[[ -f "$CERT" ]] || CERT="$ICA_DIR/newcerts/${SERIAL}.pem"
[[ -f "$CERT" ]] || die "找不到证书文件 $CERT"

# ---------- 2. 吊销 ----------
openssl ca -config "$ICA_DIR/openssl.cnf" -revoke "$CERT" \
  -crl_reason "$REASON" -batch 2>/dev/null
info "已吊销 serial=$SERIAL_UP (reason=$REASON)"

# ---------- 3. 台账 ----------
awk -F, -v s="$SERIAL_UP" -v d="$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
  'BEGIN{OFS=","} { if (toupper($1)==s) $8="revoked:"d; print }' \
  "$INVENTORY" > "$INVENTORY.tmp" && mv -f "$INVENTORY.tmp" "$INVENTORY"

# ---------- 4. 重新生成并发布 CRL ----------
publish_crl

# ---------- 5. 网关生效 ----------
reload_gateway

echo
echo "=== 吊销完成 ==="
echo "  吊销列表 : $ICA_DIR/crl.pem"
echo "  发布位置 : $CLIENT_CA_DIR/crl.pem （+ CRL_URL 上的 HTTP 副本）"
echo "  验证     : openssl crl -in $CLIENT_CA_DIR/crl.pem -noout -text | grep -A1 $SERIAL_UP"
echo "  实测     : python3 verify.py --host <网关> --port 8443 --tls \\"
echo "             --ca /etc/ssl/certs/ca-certificates.crt \\"
echo "             --cert $ICA_DIR/users/$UID_/user.crt --key $ICA_DIR/users/$UID_/user.key"
echo "             → 应握手失败（handshake failure / certificate revoked）"
