#!/usr/bin/env bash
# ============================================================
# 方案 B 裸机：安装 mTLS 网关（apt 安装全部依赖，不使用容器）
#
# 用法： sudo ./install-tls.sh [--backend nginx|stunnel] [--crl-url URL]
#
# 做了什么：
#   1. apt 安装 nginx / stunnel4 / openssl / certbot
#   2. 能力探测：nginx 是否带 stream_ssl_module，不带则自动切 stunnel
#   3. 建立目录与权限（/etc/vcc-tls/server|client-ca，/opt/vcc-ca）
#   4. 部署网关配置 + CRL 发布站点
#   5. 装 CRL 续签的 systemd timer
#   6. 若证书未就绪，给出下一步命令（不臆造证书）
# ============================================================
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=ca-lib.sh
source "$HERE/ca-lib.sh"

BACKEND="auto"
CRL_URL="http://127.0.0.1:8080/vcc-client-ca.crl"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --backend) BACKEND="${2:-}"; shift 2 ;;
    --crl-url) CRL_URL="${2:-}"; shift 2 ;;
    *) die "未知参数: $1" ;;
  esac
done

[[ $EUID -eq 0 ]] || die "请用 sudo 运行"

echo "== 1/6 apt 安装依赖 =="
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y openssl nginx stunnel4 netcat-openbsd rsync >/dev/null
apt-get install -y certbot >/dev/null 2>&1 || true
info "已安装：openssl nginx stunnel4 certbot"

echo "== 2/6 探测 nginx stream 能力 =="
HAS_STREAM=0
if nginx -V 2>&1 | grep -q 'with-stream_ssl_module'; then HAS_STREAM=1; fi
if [[ "$BACKEND" == "auto" ]]; then
  if [[ $HAS_STREAM -eq 1 ]]; then BACKEND="nginx"; else BACKEND="stunnel"; fi
fi
info "nginx stream_ssl_module: $HAS_STREAM  → 选用后端: $BACKEND"
if [[ "$BACKEND" == "nginx" && $HAS_STREAM -eq 0 ]]; then
  die "指定 nginx 但该 nginx 没有 stream_ssl 模块，请改用 --backend stunnel"
fi

echo "== 3/6 建立目录与权限 =="
install -d -m 0755 "$TLS_DIR"
install -d -m 0755 "$SERVER_CERT_DIR"
install -d -m 0755 "$CLIENT_CA_DIR"
install -d -m 0700 "$CA_DIR"
install -d -m 0755 /var/www/html
install -d -m 0755 "$HERE"
install -m 0755 "$HERE"/ca-*.sh "$HERE"/issue-user.sh "$HERE"/revoke-user.sh "$HERE"/crl-renew.sh /opt/vcc-ca/ 2>/dev/null || {
  install -d -m 0755 /opt/vcc-ca
  install -m 0755 "$HERE"/ca-lib.sh "$HERE"/ca-init.sh "$HERE"/issue-user.sh \
                  "$HERE"/revoke-user.sh "$HERE"/crl-renew.sh "$HERE"/server-cert-acme.sh /opt/vcc-ca/
}
info "脚本已安装到 /opt/vcc-ca/"

echo "== 4/6 部署网关配置 =="
if [[ "$BACKEND" == "nginx" ]]; then
  [[ -f /etc/nginx/nginx.conf.orig ]] || cp -a /etc/nginx/nginx.conf /etc/nginx/nginx.conf.orig
  install -m 0644 "$HERE/nginx-vcc-tls.conf" /etc/nginx/nginx.conf
  nginx -t && info "nginx 配置校验通过"
  systemctl enable --now nginx
else
  install -m 0644 "$HERE/stunnel-server.conf" /etc/stunnel/vcc.conf
  sed -i 's/^ENABLED=.*/ENABLED=1/' /etc/default/stunnel4 2>/dev/null || true
  systemctl enable --now stunnel4 || systemctl enable --now stunnel
fi

echo "== 5/6 CRL 续签定时器 =="
install -m 0644 "$HERE/systemd/vcc-crl.service" /etc/systemd/system/vcc-crl.service
install -m 0644 "$HERE/systemd/vcc-crl.timer"   /etc/systemd/system/vcc-crl.timer
systemctl daemon-reload
systemctl enable --now vcc-crl.timer
info "CRL 每日续签定时器已启用： systemctl list-timers vcc-crl*"

echo "== 6/6 证书状态检查 =="
if [[ -f "$SERVER_CERT_DIR/fullchain.pem" && -f "$CLIENT_CA_DIR/chain.crt" ]]; then
  info "证书已就绪，reload 网关"
  GATEWAY="$BACKEND" reload_gateway
else
  echo
  echo "⚠ 证书尚未就绪，请依次执行："
  echo "   1) 服务端证书（公共 CA）："
  echo "      /opt/vcc-ca/server-cert-acme.sh --domain <你的域名> --manual"
  echo "      或： --import-cert fullchain.pem --import-key privkey.pem"
  echo "   2) 客户端 CA（复用原自签 CA 降级）："
  echo "      CA_DIR=$CA_DIR TLS_DIR=$TLS_DIR /opt/vcc-ca/ca-init.sh \\"
  echo "          --reuse-root /path/to/old/certs --crl-url $CRL_URL"
  echo "   3) 按人签发："
  echo "      /opt/vcc-ca/issue-user.sh --uid u001 --name 姓名 --email 邮箱"
fi

echo
echo "=== 安装完成 ==="
echo "  后端        : $BACKEND（8443 → 127.0.0.1:8000）"
echo "  服务端证书  : $SERVER_CERT_DIR/fullchain.pem  (公共 CA)"
echo "  客户端 CA   : $CLIENT_CA_DIR/chain.crt        (自有 CA)"
echo "  CRL         : $CLIENT_CA_DIR/crl.pem  发布地址 $CRL_URL"
