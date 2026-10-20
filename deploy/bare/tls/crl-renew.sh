#!/usr/bin/env bash
# ============================================================
# CRL 定期续签（刷新 nextUpdate，防止 CRL 过期导致所有客户端连不上）
#
# 用法： sudo ./crl-renew.sh
# 建议： systemd timer 每日执行（见 systemd/vcc-crl.timer）
#       或 crontab： 17 3 * * * /opt/vcc-ca/crl-renew.sh >> /var/log/vcc-crl.log 2>&1
# ============================================================
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=ca-lib.sh
source "$HERE/ca-lib.sh"

echo "[$(date -u +%FT%TZ)] CRL 续签开始"
publish_crl
reload_gateway

# 若配置了 CRL 的 HTTP 发布目录，同步一份（供客户端/其他网关拉取）
CRL_PUB_DIR="${CRL_PUB_DIR:-/var/www/html}"
if [[ -d "$CRL_PUB_DIR" ]]; then
  install -m 0644 "$CLIENT_CA_DIR/crl.pem" "$CRL_PUB_DIR/vcc-client-ca.crl"
  info "已发布 HTTP 副本：$CRL_PUB_DIR/vcc-client-ca.crl"
fi

NEXT="$(openssl crl -in "$CLIENT_CA_DIR/crl.pem" -noout -nextupdate 2>/dev/null | sed 's/nextUpdate=//')"
echo "[$(date -u +%FT%TZ)] CRL 续签完成，下次更新：$NEXT"
