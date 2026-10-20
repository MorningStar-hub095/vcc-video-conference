#!/usr/bin/env bash
# ============================================================
# 服务端证书：改用「公共可信 CA」签发（Let's Encrypt / 其他商业 CA）
#
# 客户端（stunnel）侧因此可以直接用系统信任库校验服务端，
# 不再需要下发自签根 CA —— 防中间人、且免去客户端信任配置。
#
# 三种用法：
#   A. DNS-01（推荐，无公网 80 端口也可用，支持泛域名）
#      sudo ./server-cert-acme.sh --domain meet.example.com --dns-plugin dns_aliyun
#   B. 手工 DNS-01（任何 DNS 服务商都可用，脚本会提示加 TXT 记录）
#      sudo ./server-cert-acme.sh --domain meet.example.com --manual
#   C. 自带证书（商业 CA 下发的 fullchain + 私钥）
#      sudo ./server-cert-acme.sh --import-cert /tmp/fullchain.pem --import-key /tmp/privkey.pem
#
# 部署位置： /etc/vcc-tls/server/{fullchain.pem,privkey.pem}
# 权限：     privkey.pem 0600 root:root ；fullchain.pem 0644
# ============================================================
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=ca-lib.sh
source "$HERE/ca-lib.sh"

DOMAIN=""; EMAIL=""; DNS_PLUGIN=""; MANUAL=0
IMPORT_CERT=""; IMPORT_KEY=""; STAGING=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --domain)      DOMAIN="${2:-}";      shift 2 ;;
    --email)       EMAIL="${2:-}";       shift 2 ;;
    --dns-plugin)  DNS_PLUGIN="${2:-}";  shift 2 ;;
    --manual)      MANUAL=1;             shift ;;
    --import-cert) IMPORT_CERT="${2:-}"; shift 2 ;;
    --import-key)  IMPORT_KEY="${2:-}";  shift 2 ;;
    --staging)     STAGING=1;            shift ;;
    *) die "未知参数: $1" ;;
  esac
done

mkdir -p "$SERVER_CERT_DIR"
chmod 0755 "$TLS_DIR" "$SERVER_CERT_DIR"

if [[ -n "$IMPORT_CERT" ]]; then
  [[ -f "$IMPORT_CERT" && -f "$IMPORT_KEY" ]] || die "证书或私钥文件不存在"
  install -m 0644 "$IMPORT_CERT" "$SERVER_CERT_DIR/fullchain.pem"
  install -m 0600 "$IMPORT_KEY"  "$SERVER_CERT_DIR/privkey.pem"
  info "已导入公共 CA 证书：$SERVER_CERT_DIR/fullchain.pem"
else
  [[ -n "$DOMAIN" ]] || die "请给 --domain（或 --import-cert/--import-key）"
  command -v certbot >/dev/null 2>&1 || {
    info "安装 certbot..."; apt-get update -qq
    apt-get install -y certbot >/dev/null
  }
  ARGS=(certonly --non-interactive --agree-tos --keep-until-expiring
        --key-type rsa --rsa-key-size 2048
        --cert-name vcc-server -d "$DOMAIN")
  [[ -n "$EMAIL" ]] && ARGS+=(-m "$EMAIL" --no-eff-email) || ARGS+=(--register-unsafely-without-email)
  [[ $STAGING -eq 1 ]] && ARGS+=(--staging)

  if [[ -n "$DNS_PLUGIN" ]]; then
    apt-get install -y "python3-certbot-dns-${DNS_PLUGIN#dns_}" >/dev/null 2>&1 || true
    ARGS+=(--authenticator "$DNS_PLUGIN")
  elif [[ $MANUAL -eq 1 ]]; then
    ARGS+=(--manual --preferred-challenges dns)
    ARGS+=(--manual-auth-hook /bin/true)   # 手工模式：脚本会暂停等你加 TXT
  else
    ARGS+=(--authenticator webroot --webroot-path /var/www/html)
  fi

  certbot "${ARGS[@]}"

  install -m 0644 "/etc/letsencrypt/live/vcc-server/fullchain.pem" "$SERVER_CERT_DIR/fullchain.pem"
  install -m 0600 "/etc/letsencrypt/live/vcc-server/privkey.pem"   "$SERVER_CERT_DIR/privkey.pem"
  info "已部署 Let's Encrypt 证书"

  # 续期后自动同步到网关目录并 reload
  cat > /etc/letsencrypt/renewal-hooks/deploy/vcc-tls.sh <<'EOS'
#!/bin/sh
set -e
install -m 0644 /etc/letsencrypt/live/vcc-server/fullchain.pem /etc/vcc-tls/server/fullchain.pem
install -m 0600 /etc/letsencrypt/live/vcc-server/privkey.pem   /etc/vcc-tls/server/privkey.pem
systemctl reload nginx 2>/dev/null || systemctl restart stunnel4 2>/dev/null || true
EOS
  chmod 0755 /etc/letsencrypt/renewal-hooks/deploy/vcc-tls.sh
  info "已配置续期钩子：renewal-hooks/deploy/vcc-tls.sh"
  systemctl enable --now certbot.timer 2>/dev/null || true
fi

# 校验：证书链能被系统信任库验证
if openssl verify -CAfile /etc/ssl/certs/ca-certificates.crt "$SERVER_CERT_DIR/fullchain.pem" >/dev/null 2>&1; then
  info "✓ 服务端证书可被系统信任库验证"
else
  info "⚠ 系统信任库校验未通过（自签或中间证书缺失时会出现），请检查 fullchain 是否完整"
fi
openssl x509 -in "$SERVER_CERT_DIR/fullchain.pem" -noout -subject -issuer -dates

reload_gateway
echo
echo "=== 服务端证书部署完成 ==="
echo "  $SERVER_CERT_DIR/fullchain.pem  0644"
echo "  $SERVER_CERT_DIR/privkey.pem    0600 root:root"
echo "  客户端 stunnel 侧改为： CAfile = /etc/ssl/certs/ca-certificates.crt（系统信任库）"
