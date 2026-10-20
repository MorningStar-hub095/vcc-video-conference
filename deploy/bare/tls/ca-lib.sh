#!/usr/bin/env bash
# ============================================================
# 客户端 CA 公共库（被 ca-init / issue-user / revoke-user / crl-renew 引用）
#
# 关键路径（可用环境变量覆盖，便于在测试环境演练）：
#   CA_DIR   默认 /opt/vcc-ca     —— CA 状态与私钥（高敏感）
#   TLS_DIR  默认 /etc/vcc-tls    —— 网关只读的证书/CRL 发布目录
# ============================================================

CA_DIR="${CA_DIR:-/opt/vcc-ca}"
TLS_DIR="${TLS_DIR:-/etc/vcc-tls}"

ROOT_DIR="$CA_DIR/root"                 # 根 CA（生产环境应离线/不在网关机上）
ICA_DIR="$CA_DIR/intermediate"          # 签发 CA（真正签用户证书）
CLIENT_CA_DIR="$TLS_DIR/client-ca"      # 网关读取：chain.crt / crl.pem
SERVER_CERT_DIR="$TLS_DIR/server"       # 网关读取：fullchain.pem / privkey.pem
DIST_DIR="$CA_DIR/distribute"           # 待分发的 p12
INVENTORY="$ICA_DIR/inventory.csv"
CRL_URL_FILE="$CA_DIR/crl-url.txt"

CRL_DAYS="${CRL_DAYS:-30}"
GATEWAY="${GATEWAY:-auto}"              # nginx | stunnel | auto | none

die() { echo "✗ $*" >&2; exit 1; }
info() { echo "  $*"; }

need_file() { [[ -f "$1" ]] || die "缺少文件：$1（先跑 ca-init.sh）"; }

read_crl_url() {
  [[ -f "$CRL_URL_FILE" ]] && cat "$CRL_URL_FILE" || echo "http://127.0.0.1:8080/vcc-client-ca.crl"
}

publish_crl() {
  # 重新生成根 CA 与签发 CA 的 CRL，并合并发布
  # 注意：nginx 校验客户端证书链时，链上每个 CA 都要有对应 CRL，
  #       所以根 CA 的 CRL 也必须一起放进 crl.pem，否则会报
  #       "unable to get certificate CRL"。
  need_file "$ROOT_DIR/openssl.cnf"
  need_file "$ICA_DIR/openssl.cnf"

  openssl ca -config "$ROOT_DIR/openssl.cnf" -gencrl \
      -crldays "$CRL_DAYS" -out "$ROOT_DIR/crl.pem" -batch 2>/dev/null
  openssl ca -config "$ICA_DIR/openssl.cnf" -gencrl \
      -crldays "$CRL_DAYS" -out "$ICA_DIR/crl.pem" -batch 2>/dev/null

  cat "$ICA_DIR/crl.pem" "$ROOT_DIR/crl.pem" > "$CLIENT_CA_DIR/crl.pem"
  chmod 0644 "$CLIENT_CA_DIR/crl.pem"
  info "CRL 已发布：$CLIENT_CA_DIR/crl.pem"
  info "  签发 CA CRL nextUpdate: $(openssl crl -in "$ICA_DIR/crl.pem" -noout -nextupdate 2>/dev/null | sed 's/nextUpdate=//')"
  info "  吊销条目数: $(openssl crl -in "$ICA_DIR/crl.pem" -noout -text 2>/dev/null | grep -c 'Serial Number:')"
}

reload_gateway() {
  case "$GATEWAY" in
    none) info "GATEWAY=none，跳过重载（请手动 reload）"; return 0 ;;
    nginx)
      systemctl reload nginx 2>/dev/null || systemctl restart nginx 2>/dev/null \
        || die "nginx 重载失败，请手动： systemctl reload nginx"
      info "nginx 已重载"; return 0 ;;
    stunnel)
      systemctl restart stunnel4 2>/dev/null || systemctl restart stunnel 2>/dev/null \
        || die "stunnel 重载失败"
      info "stunnel 已重启"; return 0 ;;
    auto)
      if systemctl is-active --quiet nginx 2>/dev/null; then
        GATEWAY=nginx; reload_gateway
      elif systemctl is-active --quiet stunnel4 2>/dev/null; then
        GATEWAY=stunnel; reload_gateway
      else
        info "未检测到运行中的网关，跳过重载"
      fi ;;
  esac
}
