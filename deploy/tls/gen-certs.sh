#!/usr/bin/env bash
# ============================================================
# 生成 mTLS 证书体系（自签 CA + 服务端证书 + 客户端证书）
#
# 用途：服务端源码是三路明文 TCP 且按约束不得改动，
#       因此在「网关层」做 TLS 终结 + 客户端证书鉴权（mTLS），
#       应用侧零改动。
#
# 产物（deploy/certs/，与 docker-compose.tls.yml 的挂载点一致）：
#   ca.key / ca.crt        根 CA（务必离线保管）
#   server.key / server.crt  服务端证书（nginx 网关用）
#   client.key / client.crt  客户端证书（stunnel / 网关鉴权用）
#   client.p12              客户端证书+私钥（Windows 导入用，口令见 CERT_PWD）
#
# 用法： ./gen-certs.sh [服务端域名或IP，默认 localhost]
# ============================================================
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/../certs"
SERVER_CN="${1:-localhost}"
#【P2修复：p12 导出口令硬编码 "vcc"】
# 修改原因: 默认口令 "vcc" 随源码公开，client.p12 内含私钥，等于私钥裸奔；
#           任何人拿到仓库都能导出客户端证书通过 mTLS 鉴权。
# 修改效果: 未显式传入 CERT_PWD 时自动生成 24 位随机口令，写入
#           $OUT/client.p12.password（chmod 600，不入库），并在末尾提示；
#           显式传入 CERT_PWD 时行为不变。
# 原代码保留备查：
# CERT_PWD="${CERT_PWD:-vcc}"
if [ -z "${CERT_PWD:-}" ]; then
  CERT_PWD="$(openssl rand -base64 24 | tr -d '/+=\n' | cut -c1-24)"
  GENERATED_PWD=1
else
  GENERATED_PWD=0
fi
DAYS_CA=3650
DAYS_LEAF=825

mkdir -p "$OUT"

echo "== 1/5 生成根 CA =="
openssl req -x509 -newkey rsa:4096 -sha256 -days "$DAYS_CA" -nodes \
  -keyout "$OUT/ca.key" -out "$OUT/ca.crt" \
  -subj "/C=CN/O=VCCMeeting/OU=Deploy/CN=VCC-Deploy-CA" \
  -addext "basicConstraints=critical,CA:TRUE" \
  -addext "keyUsage=critical,keyCertSign,cRLSign" 2>/dev/null

echo "== 2/5 生成服务端私钥与 CSR =="
openssl req -newkey rsa:2048 -nodes \
  -keyout "$OUT/server.key" -out "$OUT/server.csr" \
  -subj "/C=CN/O=VCCMeeting/OU=Server/CN=${SERVER_CN}" 2>/dev/null

# SAN：同时支持 IP 与 DNS，避免客户端校验主机名失败
#【P2修复：TLS SAN 缺 IP 条目】
# 修改原因: 原固定只写 IP.1=127.0.0.1；当 SERVER_CN 是 IP（如 192.0.2.10，示例用 TEST-NET 地址）时
#           它被塞进 DNS.1，客户端按 IP 直连时主机名校验必失败。
# 修改效果: 判断 SERVER_CN 形态——IP 则写入 IP 条目，域名则写入 DNS 条目。
# 原代码保留备查：
#   DNS.1=${SERVER_CN} / DNS.2=localhost / IP.1=127.0.0.1
if echo "$SERVER_CN" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$'; then
  SAN_EXTRA="IP.2=${SERVER_CN}"
else
  SAN_EXTRA="DNS.2=${SERVER_CN}"
fi
cat > "$OUT/server-ext.cnf" <<EOF
basicConstraints=CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=@alt
[alt]
DNS.1=localhost
${SAN_EXTRA}
IP.1=127.0.0.1
EOF

echo "== 3/5 用 CA 签发服务端证书 =="
openssl x509 -req -in "$OUT/server.csr" -CA "$OUT/ca.crt" -CAkey "$OUT/ca.key" \
  -CAcreateserial -out "$OUT/server.crt" -days "$DAYS_LEAF" -sha256 \
  -extfile "$OUT/server-ext.cnf" 2>/dev/null

echo "== 4/5 生成客户端证书（用于 mTLS 鉴权）=="
openssl req -newkey rsa:2048 -nodes \
  -keyout "$OUT/client.key" -out "$OUT/client.csr" \
  -subj "/C=CN/O=VCCMeeting/OU=Client/CN=vcc-client" 2>/dev/null
cat > "$OUT/client-ext.cnf" <<EOF
basicConstraints=CA:FALSE
keyUsage=critical,digitalSignature
extendedKeyUsage=clientAuth
EOF
openssl x509 -req -in "$OUT/client.csr" -CA "$OUT/ca.crt" -CAkey "$OUT/ca.key" \
  -CAcreateserial -out "$OUT/client.crt" -days "$DAYS_LEAF" -sha256 \
  -extfile "$OUT/client-ext.cnf" 2>/dev/null

echo "== 5/5 打包客户端证书为 PKCS#12（Windows 导入用）=="
openssl pkcs12 -export -out "$OUT/client.p12" \
  -inkey "$OUT/client.key" -in "$OUT/client.crt" -certfile "$OUT/ca.crt" \
  -passout "pass:${CERT_PWD}"

rm -f "$OUT"/*.csr "$OUT"/*.cnf "$OUT"/ca.srl
chmod 600 "$OUT"/*.key
chmod 644 "$OUT"/*.crt

#【P2修复：p12 导出口令硬编码】自动生成口令时落盘保存（私钥文件同等级保护）
if [ "$GENERATED_PWD" = "1" ]; then
  printf '%s\n' "$CERT_PWD" > "$OUT/client.p12.password"
  chmod 600 "$OUT/client.p12.password"
fi

echo
echo "=== 生成完成：$OUT ==="
ls -l "$OUT"
echo
#【P2修复：p12 导出口令硬编码】提示口令位置（自动生成模式下）
if [ "$GENERATED_PWD" = "1" ]; then
  echo "client.p12 导入口令已随机生成并保存到: $OUT/client.p12.password（请离线保管）"
  echo "导入时使用: \$(cat $OUT/client.p12.password)"
else
  echo "client.p12 导入口令: 使用你显式指定的 CERT_PWD"
fi
echo
echo "校验： openssl verify -CAfile $OUT/ca.crt $OUT/server.crt $OUT/client.crt"
openssl verify -CAfile "$OUT/ca.crt" "$OUT/server.crt" "$OUT/client.crt"
echo
echo "提示：certs/ 含私钥，已在 .gitignore 中排除，切勿入库。"
