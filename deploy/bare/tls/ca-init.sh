#!/usr/bin/env bash
# ============================================================
# 客户端证书体系初始化（根 CA + 签发 CA）
#
# 定位：服务端证书已改用「公共可信 CA」，本 CA **只用于签发客户端证书**，
#       不再签发任何服务端证书。原有自签 CA 通过 --reuse-root 直接降级复用。
#
# 用法：
#   sudo ./ca-init.sh                                  # 全新创建根 CA + 签发 CA
#   sudo ./ca-init.sh --reuse-root /path/to/old/certs  # 复用原自签 CA 当根 CA
#                     --crl-url http://host/vcc-client-ca.crl
#
# 可覆盖： CA_DIR / TLS_DIR / CRL_DAYS
# ============================================================
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=ca-lib.sh
source "$HERE/ca-lib.sh"

REUSE_ROOT=""
CRL_URL=""
ROOT_CN="VCC-Client-Root-CA"
ICA_CN="VCC-Client-Issuing-CA"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --reuse-root) REUSE_ROOT="${2:-}"; shift 2 ;;
    --crl-url)    CRL_URL="${2:-}";    shift 2 ;;
    --root-cn)    ROOT_CN="${2:-}";    shift 2 ;;
    --ica-cn)     ICA_CN="${2:-}";     shift 2 ;;
    --crl-days)   CRL_DAYS="${2:-}";   shift 2 ;;
    *) die "未知参数: $1" ;;
  esac
done

CRL_URL="${CRL_URL:-$(read_crl_url)}"

echo "== 客户端 CA 初始化 =="
info "CA_DIR  = $CA_DIR"
info "TLS_DIR = $TLS_DIR"
info "CRL 发布地址 = $CRL_URL"

mkdir -p "$ROOT_DIR/newcerts" "$ICA_DIR/newcerts" "$ICA_DIR/users" "$DIST_DIR" \
         "$CLIENT_CA_DIR" "$SERVER_CERT_DIR"
chmod 0700 "$CA_DIR" "$ROOT_DIR" "$ICA_DIR"

# ---------- 1. 根 CA ----------
if [[ -f "$ROOT_DIR/ca.crt" && -f "$ROOT_DIR/ca.key" ]]; then
  info "根 CA 已存在，跳过创建"
elif [[ -n "$REUSE_ROOT" ]]; then
  [[ -f "$REUSE_ROOT/ca.crt" ]] || die "$REUSE_ROOT/ca.crt 不存在"
  [[ -f "$REUSE_ROOT/ca.key" ]] || die "$REUSE_ROOT/ca.key 不存在"
  cp -f "$REUSE_ROOT/ca.crt" "$ROOT_DIR/ca.crt"
  cp -f "$REUSE_ROOT/ca.key" "$ROOT_DIR/ca.key"
  info "已复用原自签 CA 作为根 CA：$REUSE_ROOT"
  info "⚠ 该 CA 自此只签发客户端证书，不再签发服务端证书"
else
  openssl req -x509 -newkey rsa:4096 -sha256 -days 3650 -nodes \
    -keyout "$ROOT_DIR/ca.key" -out "$ROOT_DIR/ca.crt" \
    -subj "/O=VCCMeeting/OU=Client-PKI/CN=${ROOT_CN}" \
    -addext "basicConstraints=critical,CA:TRUE" \
    -addext "keyUsage=critical,keyCertSign,cRLSign" 2>/dev/null
  info "已创建根 CA（RSA 4096 / 10 年）"
fi
chmod 0400 "$ROOT_DIR/ca.key"
chmod 0644 "$ROOT_DIR/ca.crt"

# ---------- 2. 根 CA 数据库 ----------
touch "$ROOT_DIR/index.txt"
[[ -f "$ROOT_DIR/serial" ]]    || echo "1000" > "$ROOT_DIR/serial"
[[ -f "$ROOT_DIR/crlnumber" ]] || echo "1000" > "$ROOT_DIR/crlnumber"

cat > "$ROOT_DIR/openssl.cnf" <<EOF
[ ca ]
default_ca = CA_default

[ CA_default ]
dir               = $ROOT_DIR
database          = \$dir/index.txt
serial            = \$dir/serial
crlnumber         = \$dir/crlnumber
new_certs_dir     = \$dir/newcerts
certificate       = \$dir/ca.crt
private_key       = \$dir/ca.key
default_md        = sha256
default_days      = 3650
default_crl_days  = $CRL_DAYS
preserve          = no
email_in_dn       = no
unique_subject    = no
copy_extensions   = none
policy            = policy_loose

[ policy_loose ]
countryName             = optional
stateOrProvinceName     = optional
localityName            = optional
organizationName        = optional
organizationalUnitName  = optional
commonName              = supplied
emailAddress            = optional

[ v3_intermediate ]
subjectKeyIdentifier   = hash
authorityKeyIdentifier = keyid:always,issuer
basicConstraints       = critical, CA:true, pathlen:0
keyUsage               = critical, keyCertSign, cRLSign
crlDistributionPoints  = URI:$CRL_URL
EOF

# ---------- 3. 签发 CA（中间 CA）----------
if [[ -f "$ICA_DIR/ica.crt" ]]; then
  info "签发 CA 已存在，跳过创建"
else
  openssl req -newkey rsa:4096 -nodes -sha256 \
    -keyout "$ICA_DIR/ica.key" -out "$ICA_DIR/ica.csr" \
    -subj "/O=VCCMeeting/OU=Client-PKI/CN=${ICA_CN}" 2>/dev/null
  openssl ca -config "$ROOT_DIR/openssl.cnf" -batch -notext -md sha256 \
    -extensions v3_intermediate -days 3650 \
    -in "$ICA_DIR/ica.csr" -out "$ICA_DIR/ica.crt" 2>/dev/null
  info "已创建签发 CA（由根 CA 签发，pathlen:0）"
fi
chmod 0600 "$ICA_DIR/ica.key"
chmod 0644 "$ICA_DIR/ica.crt"

# ---------- 4. 签发 CA 数据库 ----------
touch "$ICA_DIR/index.txt"
[[ -f "$ICA_DIR/serial" ]]    || echo "1000" > "$ICA_DIR/serial"
[[ -f "$ICA_DIR/crlnumber" ]] || echo "1000" > "$ICA_DIR/crlnumber"

cat > "$ICA_DIR/openssl.cnf" <<EOF
[ ca ]
default_ca = CA_default

[ CA_default ]
dir               = $ICA_DIR
database          = \$dir/index.txt
serial            = \$dir/serial
crlnumber         = \$dir/crlnumber
new_certs_dir     = \$dir/newcerts
certificate       = \$dir/ica.crt
private_key       = \$dir/ica.key
default_md        = sha256
default_days      = 365
default_crl_days  = $CRL_DAYS
preserve          = no
email_in_dn       = yes
unique_subject    = no
copy_extensions   = none
policy            = policy_user

[ policy_user ]
countryName             = optional
stateOrProvinceName     = optional
localityName            = optional
organizationName        = supplied
organizationalUnitName  = optional
commonName              = supplied
emailAddress            = supplied

[ v3_client ]
subjectKeyIdentifier   = hash
authorityKeyIdentifier = keyid,issuer
basicConstraints       = critical, CA:FALSE
keyUsage               = critical, digitalSignature, keyEncipherment
extendedKeyUsage       = clientAuth
crlDistributionPoints  = URI:$CRL_URL
EOF

# ---------- 5. 发布信任链 ----------
cat "$ICA_DIR/ica.crt" "$ROOT_DIR/ca.crt" > "$CLIENT_CA_DIR/chain.crt"
cp -f "$ROOT_DIR/ca.crt" "$CLIENT_CA_DIR/root-ca.crt"
cp -f "$ICA_DIR/ica.crt" "$CLIENT_CA_DIR/issuing-ca.crt"
chmod 0644 "$CLIENT_CA_DIR"/*.crt
echo "$CRL_URL" > "$CRL_URL_FILE"

[[ -f "$INVENTORY" ]] || \
  echo "serial,uid,name,email,issued,expires,fingerprint,status" > "$INVENTORY"
chmod 0640 "$INVENTORY"

# ---------- 6. 初始 CRL ----------
publish_crl

echo
echo "=== 初始化完成 ==="
echo "  根 CA      : $ROOT_DIR/ca.crt   (私钥 $ROOT_DIR/ca.key, 0400)"
echo "  签发 CA    : $ICA_DIR/ica.crt   (私钥 $ICA_DIR/ica.key, 0600)"
echo "  网关信任链 : $CLIENT_CA_DIR/chain.crt  (ica + root)"
echo "  网关 CRL   : $CLIENT_CA_DIR/crl.pem"
echo "  签发用户   : ./issue-user.sh --uid u001 --name 姓名 --email 邮箱"
echo
echo "⚠ 生产环境请把根 CA 私钥（$ROOT_DIR/ca.key）离线保存并从网关机删除；"
echo "  网关机只需保留 chain.crt / crl.pem / 签发 CA。"
