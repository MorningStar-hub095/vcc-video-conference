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
CERT_PWD="${CERT_PWD:-vcc}"
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
cat > "$OUT/server-ext.cnf" <<EOF
basicConstraints=CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=@alt
[alt]
DNS.1=${SERVER_CN}
DNS.2=localhost
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

echo
echo "=== 生成完成：$OUT ==="
ls -l "$OUT"
echo
echo "校验： openssl verify -CAfile $OUT/ca.crt $OUT/server.crt $OUT/client.crt"
openssl verify -CAfile "$OUT/ca.crt" "$OUT/server.crt" "$OUT/client.crt"
echo
echo "提示：certs/ 含私钥，已在 .gitignore 中排除，切勿入库。"
