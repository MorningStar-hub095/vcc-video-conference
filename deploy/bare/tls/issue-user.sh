#!/usr/bin/env bash
# ============================================================
# 按人签发客户端证书（每人一套私钥 + 证书）
#
# 证书主体体现使用者标识：
#   O=VCCMeeting  OU=Client  CN=<姓名>  UID=<工号/账号>  emailAddress=<邮箱>
#   扩展： subjectAltName=email:<邮箱> ；extendedKeyUsage=clientAuth
#
# 用法：
#   sudo ./issue-user.sh --uid u001 --name 张三 --email zhangsan@example.com
#                        [--days 365] [--p12-pass 一次性口令] [--no-keep]
#   --no-keep  打包后立即销毁服务端私钥副本（推荐用于真实分发）
# ============================================================
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=ca-lib.sh
source "$HERE/ca-lib.sh"

UID_=""; NAME=""; EMAIL=""; DAYS=365; P12PASS=""; KEEP=1

while [[ $# -gt 0 ]]; do
  case "$1" in
    --uid)      UID_="${2:-}";  shift 2 ;;
    --name)     NAME="${2:-}";  shift 2 ;;
    --email)    EMAIL="${2:-}"; shift 2 ;;
    --days)     DAYS="${2:-}";  shift 2 ;;
    --p12-pass) P12PASS="${2:-}"; shift 2 ;;
    --no-keep)  KEEP=0; shift ;;
    *) die "未知参数: $1" ;;
  esac
done

[[ -n "$UID_" && -n "$NAME" && -n "$EMAIL" ]] || \
  die "用法: issue-user.sh --uid <工号> --name <姓名> --email <邮箱>"

#【P2修复：subject 未转义】
# 修改原因: NAME/EMAIL/UID_ 直接拼进 -subj 字符串，值里含 '/' 会被 openssl 当成
#           字段分隔符——既能截断 subject，也能注入任意 RDN（如冒充 O=/OU=）。
# 修改效果: 三个值中出现 '/' 直接拒绝签发；其余合法字符行为不变。
#           （openssl -subj 无官方转义机制，白名单拒绝是唯一可靠做法。）
# 原代码: （无校验，直接拼入 -subj）
for _v in "$NAME" "$EMAIL" "$UID_"; do
  [[ "$_v" != *"/"* ]] || die "参数含非法字符 '/'，拒绝签发（防 subject 注入）: $_v"
done
need_file "$ICA_DIR/openssl.cnf"
need_file "$ICA_DIR/ica.crt"

CRL_URL="$(read_crl_url)"
USER_DIR="$ICA_DIR/users/$UID_"
mkdir -p "$USER_DIR" "$DIST_DIR"
chmod 0700 "$USER_DIR"

if [[ -z "$P12PASS" ]]; then
  P12PASS="$(openssl rand -base64 18 | tr -d '/+=' | head -c 16)"
fi

echo "== 签发客户端证书 =="
info "使用者 : $NAME (UID=$UID_, $EMAIL)"

# ---------- 1. 私钥 + CSR ----------
openssl req -newkey rsa:2048 -nodes -sha256 \
  -keyout "$USER_DIR/user.key" -out "$USER_DIR/user.csr" \
  -subj "/O=VCCMeeting/OU=Client/CN=${NAME}/UID=${UID_}/emailAddress=${EMAIL}" 2>/dev/null
chmod 0600 "$USER_DIR/user.key"

# ---------- 2. 扩展 ----------
# 注意：openssl ca 用了 -extfile 时，-extensions 指定的段必须在 extfile 内部，
#       否则报 "Error checking certificate extensions from extfile section"。
cat > "$USER_DIR/ext.cnf" <<EOF
[ v3_client ]
subjectKeyIdentifier   = hash
authorityKeyIdentifier = keyid,issuer
basicConstraints       = critical, CA:FALSE
keyUsage               = critical, digitalSignature, keyEncipherment
extendedKeyUsage       = clientAuth
subjectAltName         = email:${EMAIL}
crlDistributionPoints  = URI:${CRL_URL}
EOF

# ---------- 3. 签发 ----------
openssl ca -config "$ICA_DIR/openssl.cnf" -batch -notext -md sha256 \
  -extensions v3_client -extfile "$USER_DIR/ext.cnf" -days "$DAYS" \
  -in "$USER_DIR/user.csr" -out "$USER_DIR/user.crt" 2>/dev/null

SERIAL="$(openssl x509 -in "$USER_DIR/user.crt" -noout -serial | cut -d= -f2)"
FP="$(openssl x509 -in "$USER_DIR/user.crt" -noout -fingerprint -sha256 | cut -d= -f2)"
ISSUED="$(openssl x509 -in "$USER_DIR/user.crt" -noout -startdate | cut -d= -f2)"
EXPIRES="$(openssl x509 -in "$USER_DIR/user.crt" -noout -enddate | cut -d= -f2)"

# ---------- 4. 打包 p12（含信任链，便于 Windows/移动端导入）----------
openssl pkcs12 -export -macalg sha256 \
  -inkey "$USER_DIR/user.key" -in "$USER_DIR/user.crt" \
  -certfile "$CLIENT_CA_DIR/chain.crt" \
  -name "${NAME} <${EMAIL}>" \
  -out "$DIST_DIR/${UID_}.p12" -passout "pass:${P12PASS}"
chmod 0640 "$DIST_DIR/${UID_}.p12"

# ---------- 5. 台账 ----------
echo "${SERIAL},${UID_},${NAME},${EMAIL},${ISSUED},${EXPIRES},${FP},issued" >> "$INVENTORY"

echo
echo "=== 签发完成 ==="
echo "  序列号   : $SERIAL"
echo "  有效期   : $ISSUED  →  $EXPIRES"
echo "  指纹     : $FP"
echo "  证书     : $USER_DIR/user.crt"
echo "  私钥     : $USER_DIR/user.key"
echo "  分发包   : $DIST_DIR/${UID_}.p12"
echo "  一次性口令: $P12PASS   ← 通过独立渠道（当面/加密IM）告知本人，不要与 p12 同渠道发送"
echo
echo "导入（Windows）：双击 p12 → 存储位置「个人」→ 输入上述口令"
echo "导入（Linux stunnel）：直接用 user.crt + user.key，见 stunnel-client-user.conf.template"

if [[ $KEEP -eq 0 ]]; then
  shred -u "$USER_DIR/user.key" 2>/dev/null || rm -f "$USER_DIR/user.key"
  echo "  已销毁服务端私钥副本（--no-keep）"
fi
