#!/usr/bin/env bash
# ============================================================
# mTLS 配置演练（在 Docker 里跑「同一份」裸机 nginx 配置）
#
# 目的：在没法直接进虚拟机的情况下，用真实 nginx 验证这套配置：
#   - 公共 CA 签发的服务端证书（这里用一个独立根 CA 扮演公共 CA）
#   - 自有客户端 CA（复用原自签 CA 降级）→ 签发 CA → 每人一张证书
#   - CRL 生效：已吊销证书握手必须失败
#
# 用法：  cd deploy/bare/tls/lab && ./run-lab.sh
# 依赖：  docker（引擎运行中）+ openssl + python3
# ============================================================
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"

WORK="$HERE/_work"
export CA_DIR="$WORK/ca"
export TLS_DIR="$WORK/tls"
export GATEWAY=none
PUB="$WORK/public-ca"

export PATH="$PATH:/c/Program Files/Docker/Docker/resources/bin"

rm -rf "$WORK"; mkdir -p "$WORK"

echo "########## 1/6 客户端 CA（复用原自签 CA 降级为根 CA）##########"
# lab/ -> tls/ -> bare/ -> deploy/
OLD_CA="$HERE/../../../certs"
[[ -f "$OLD_CA/ca.crt" && -f "$OLD_CA/ca.key" ]] || OLD_CA=""
if [[ -n "$OLD_CA" ]]; then
  bash "$HERE/../ca-init.sh" --reuse-root "$OLD_CA" \
       --crl-url "http://127.0.0.1:8080/vcc-client-ca.crl" 2>&1 | tail -12
else
  echo "  (未找到原自签 CA，改为全新创建根 CA)"
  bash "$HERE/../ca-init.sh" \
       --crl-url "http://127.0.0.1:8080/vcc-client-ca.crl" 2>&1 | tail -12
fi

echo
echo "########## 2/6 公共 CA 签发服务端证书（演练：用独立根 CA 扮演公共 CA）##########"
mkdir -p "$PUB" "$TLS_DIR/server"
# CA 证书必须带 basicConstraints + keyUsage(keyCertSign)，
# 否则 OpenSSL 3 / Python 3.13 会拒绝：「CA cert does not include key usage extension」
openssl req -x509 -newkey rsa:2048 -sha256 -days 825 -nodes \
  -keyout "$PUB/ca.key" -out "$PUB/ca.crt" \
  -subj "/O=PublicCA-For-Lab/CN=Public CA Lab Root" \
  -addext "basicConstraints=critical,CA:TRUE" \
  -addext "keyUsage=critical,keyCertSign,cRLSign" 2>/dev/null
openssl req -newkey rsa:2048 -nodes -sha256 -keyout "$TLS_DIR/server/privkey.pem" \
  -out "$PUB/server.csr" -subj "/CN=localhost" 2>/dev/null
cat > "$PUB/ext.cnf" <<'EOF'
basicConstraints=CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:localhost,IP:127.0.0.1
EOF
openssl x509 -req -in "$PUB/server.csr" -CA "$PUB/ca.crt" -CAkey "$PUB/ca.key" \
  -CAcreateserial -out "$TLS_DIR/server/fullchain.pem" -days 825 -sha256 \
  -extfile "$PUB/ext.cnf" 2>/dev/null
chmod 0600 "$TLS_DIR/server/privkey.pem"
echo "  服务端证书: $TLS_DIR/server/fullchain.pem"
echo "  签发者    : $(openssl x509 -in "$TLS_DIR/server/fullchain.pem" -noout -issuer | sed 's/issuer=//')"

echo
echo "########## 3/6 按人签发客户端证书 ##########"
bash "$HERE/../issue-user.sh" --uid u001 --name "张三" --email "zhangsan@example.com" 2>&1 | tail -8
echo "---"
bash "$HERE/../issue-user.sh" --uid u002 --name "李四" --email "lisi@example.com" 2>&1 | tail -6

echo
echo "########## 4/6 吊销李四(u002) 并发布 CRL ##########"
bash "$HERE/../revoke-user.sh" --uid u002 2>&1 | tail -8

echo
echo "########## 5/6 启动实验室网关（复用裸机 nginx 配置）##########"
# 唯一差异：裸机转发到 127.0.0.1:8000，实验室里转发到 compose 网络内的 server:8000
# 与裸机配置的差异仅两处（容器镜像特性）：
#   1) 转发目标 127.0.0.1:8000 -> server:8000
#   2) alpine 镜像没有 www-data 用户 -> nginx
sed -e 's|server 127.0.0.1:8000|server server:8000|' \
    -e 's|^user www-data;|user nginx;|' \
    "$HERE/../nginx-vcc-tls.conf" > "$HERE/nginx-lab.conf"
docker compose -f docker-compose.lab.yml down >/dev/null 2>&1 || true
docker compose -f docker-compose.lab.yml up -d 2>&1 | tail -3
for i in $(seq 1 30); do
  sleep 2
  docker inspect -f '{{.State.Health.Status}}' vcc-tls-lab 2>/dev/null | grep -q healthy && break
done
docker ps --filter name=vcc-tls-lab --format '{{.Names}} {{.Status}} {{.Ports}}'

echo
echo "########## 6/6 验证 ##########"
U001="$CA_DIR/intermediate/users/u001"
U002="$CA_DIR/intermediate/users/u002"
LA=/tmp/lab-A.log; LB=/tmp/lab-B.log

# 注意：不能用 `cmd | tail` 后再取 $? —— 那拿到的是 tail 的退出码（永远 0），
# 会让「已吊销证书竟然通过」这种致命结论被误判成通过。必须先落盘再取退出码。
echo "--- A. 张三(u001) 正常证书 → 期望握手 + 注册 + 登录全部成功 ---"
python3 "$HERE/../../../verify.py" --host 127.0.0.1 --port 8443 --tls \
   --ca "$PUB/ca.crt" --cert "$U001/user.crt" --key "$U001/user.key" >"$LA" 2>&1
A=$?
tail -8 "$LA"

echo
echo "--- B. 李四(u002) 已吊销证书 → 期望握手被拒 ---"
python3 "$HERE/../../../verify.py" --host 127.0.0.1 --port 8443 --tls \
   --ca "$PUB/ca.crt" --cert "$U002/user.crt" --key "$U002/user.key" >"$LB" 2>&1
B=$?
tail -6 "$LB"

echo
echo "--- B2. 以网关视角取证（唯一权威判据）---"
# TLS1.3 下客户端在收到服务端 alert 之前就认为握手已完成，
# 所以「客户端说自己 OK」不算数；必须看网关 stream 访问日志里的 $ssl_client_verify。
docker exec vcc-tls-lab sh -c \
  'grep "verify=" /var/log/nginx/vcc-stream.log 2>/dev/null | tail -4' 2>/dev/null | sed 's/^/  /'
REVOKE_LOG=$(docker exec vcc-tls-lab sh -c \
  'grep -c "verify=FAILED" /var/log/nginx/vcc-stream.log 2>/dev/null' 2>/dev/null | tr -d '\r\n')
REVOKE_LOG="${REVOKE_LOG:-0}"
echo "  网关判定 FAILED 的会话数: $REVOKE_LOG"

echo
echo "--- C. CRL 内容 ---"
openssl crl -in "$TLS_DIR/client-ca/crl.pem" -noout -text 2>/dev/null | grep -E "Serial Number|Next Update" | head -6
echo "  u002 序列号: $(openssl x509 -in "$U002/user.crt" -noout -serial | sed 's/serial=//')（须出现在上面列表里）"

echo
echo "--- D. CRL HTTP 分发点 ---"
curl -s -o /tmp/lab.crl -w "HTTP %{http_code}  %{size_download} bytes\n" \
     http://127.0.0.1:8080/vcc-client-ca.crl 2>/dev/null
openssl crl -in /tmp/lab.crl -noout -issuer 2>/dev/null | head -1

echo
echo "================= 结论 ================="
[[ $A -eq 0 ]] && echo "✓ 正常证书：握手 + 注册 + 登录通过" || echo "✗ 正常证书失败（配置有误）"
[[ $B -ne 0 ]] && echo "✓ 已吊销证书：被网关拒绝（CRL 生效）" || echo "✗ 已吊销证书竟然通过了（CRL 未生效！）"
[[ "${REVOKE_LOG:-0}" -gt 0 ]] && echo "✓ 网关日志确认命中吊销判定" || echo "✗ 网关日志未出现吊销判定"
echo "清理： docker compose -f docker-compose.lab.yml down"
