#!/usr/bin/env bash
# ============================================================
# VCC 服务端 —— 一键部署（Docker Compose）
# 用法：
#   cd deploy
#   cp .env.example .env && vim .env     # 填写 DB_PWD / MYSQL_ROOT_PWD / CLIENT_SERVER_IP
#   ./deploy.sh                          # 构建并启动（明文端口只绑 127.0.0.1）
#   ./deploy.sh --tls                    # 额外拉起 8443 mTLS 网关（公网部署必选）
#   ./deploy.sh --no-build               # 仅重启，不重新构建
# ============================================================
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"

DO_BUILD=1
DO_TLS=0
for a in "$@"; do
  case "$a" in
    --no-build) DO_BUILD=0 ;;
    --tls)      DO_TLS=1  ;;
  esac
done
# 也支持环境变量： TLS=1 ./deploy.sh
[[ "${TLS:-0}" == "1" ]] && DO_TLS=1

COMPOSE_FILES=(-f docker-compose.yml)
if [[ $DO_TLS -eq 1 ]]; then
  COMPOSE_FILES+=(-f docker-compose.tls.yml)
  if [[ ! -f certs/server.crt ]]; then
    echo "== 未发现证书，自动生成 mTLS 证书 =="
    ./tls/gen-certs.sh "${TLS_CN:-127.0.0.1}"
  fi
  export TLS_MODE="${TLS_MODE:-nginx-mtls.conf}"
fi

# ---------- 0. 前置检查 ----------
command -v docker >/dev/null 2>&1 || { echo "✗ 未安装 docker"; exit 1; }
docker compose version >/dev/null 2>&1 || { echo "✗ 未安装 docker compose v2"; exit 1; }

if [[ ! -f .env ]]; then
  echo "✗ 缺少 .env，请先执行： cp .env.example .env 并填写密钥"
  exit 1
fi
set -a; source .env; set +a

: "${DB_PWD:?未设置 DB_PWD}"
: "${MYSQL_ROOT_PWD:?未设置 MYSQL_ROOT_PWD}"
if [[ "$DB_PWD" == "请改成强口令" || "$MYSQL_ROOT_PWD" == "请改成强口令" ]]; then
  echo "✗ .env 中仍是占位口令，请修改为强口令"; exit 1
fi

IMAGE_TAG="${IMAGE_TAG:-latest}"
export IMAGE_TAG

echo "== 部署目标 =="
echo "   镜像标签 : vcc-server:${IMAGE_TAG}"
echo "   服务端口 : ${SERVER_PORT:-8000}（仅绑 127.0.0.1）"
echo "   数据库   : ${DB_NAME} @ ${DB_IP}"
[[ $DO_TLS -eq 1 ]] && echo "   TLS 网关 : ${TLS_PORT:-8443} 模式=${TLS_MODE}"

# ---------- 1. 备份（回滚基线） ----------
mkdir -p backups

# 回滚锚点必须指向【本次部署前正在运行】的镜像。
# 早期版本在此处写入的是新的 IMAGE_TAG，导致 .current_tag 与 .last_good_tag
# 恒等 —— rollback.sh 会回滚到自己，空转且静默返回"成功"。
PREV_TAG="$(cat .current_tag 2>/dev/null || echo '')"
if [[ -n "$PREV_TAG" && "$PREV_TAG" != "$IMAGE_TAG" ]]; then
  echo "$PREV_TAG" > ".last_good_tag"
  echo "   回滚锚点 : vcc-server:${PREV_TAG}（本次失败时回滚到此）"
else
  echo "   回滚锚点 : 无（首次部署，或目标标签与当前相同）"
fi

if docker ps --format '{{.Names}}' | grep -q '^vcc-mysql$'; then
  TS="$(date +%Y%m%d-%H%M%S)"
  echo "== 备份现有数据库 -> backups/db-${TS}.sql =="
  #【P2修复1：备份失败继续部署】原 `|| echo "⚠ 备份失败"` 吞掉失败继续部署：
  # 修改原因: MySQL 容器在而 dump 失败说明数据库状态异常，此时继续部署一旦失败，
  #           回滚基线（备份）根本不存在，等于裸奔上线。
  # 修改效果: 备份失败立即中止部署；仅当容器不在（首次部署）才跳过备份。
  #【P2修复2：密码经命令行泄漏】mysqldump -p"$PWD" 对 ps 可见，改经 MYSQL_PWD 环境变量。
  # 原代码保留备查：
  #   docker exec vcc-mysql mysqldump -uroot -p"${MYSQL_ROOT_PWD}" \
  #       --databases "${DB_NAME}" > "backups/db-${TS}.sql" || echo "⚠ 备份失败（首次部署可忽略）"
  docker exec -e MYSQL_PWD="${MYSQL_ROOT_PWD}" vcc-mysql mysqldump -uroot \
      --databases "${DB_NAME}" > "backups/db-${TS}.sql" \
      || { echo "✗ 数据库备份失败，中止部署（回滚基线不能缺）"; exit 1; }
fi

# ---------- 2. 准备源码 ----------
./prepare.sh

# ---------- 3. 构建 ----------
if [[ $DO_BUILD -eq 1 ]]; then
  echo "== 构建镜像 =="
  # 密钥不再走 --build-arg（不进镜像层），改由 compose 的 environment 运行期注入
  docker compose build server
fi

# ---------- 4. 启动 ----------
echo "== 启动服务 =="
docker compose "${COMPOSE_FILES[@]}" up -d

# current_tag 语义是"此刻实际运行在容器里的版本"，所以 up -d 之后立刻更新——
# 与本次冒烟能否通过无关。若等到冒烟后才写，失败时 current 会仍等于旧值
# （也就是 last_good），rollback 会因"目标==当前"而拒绝执行。
echo "${IMAGE_TAG}" > ".current_tag"

# ---------- 5. 等待健康 ----------
echo "== 等待服务就绪 =="
for i in $(seq 1 60); do
  if docker inspect -f '{{.State.Health.Status}}' vcc-server 2>/dev/null | grep -q healthy; then
    echo "✓ 服务端健康"; break
  fi
  sleep 2
  [[ $i -eq 60 ]] && { echo "✗ 服务端启动超时，查看日志： docker compose logs server"; exit 1; }
done

# ---------- 6. 验证（明文走隔离栈，TLS 只验握手） ----------
# 为什么这里不能再 `verify.py --port 8000` 直连生产栈：
#   verify.py 每次运行都会真实注册一个用户，而协议里没有注销/删除接口，
#   直接怼生产库等于每次上线都往线上 t_user 灌一条垃圾数据。
#   smoke.sh 会另起一套 -p vcc-smoke 的独立栈（独立卷/独立端口），
#   跑完 down -v 彻底销毁：既验同一份镜像，又零接触生产卷。
echo "== 冒烟验证（一次性隔离栈，生产库零写入）=="
if ! ./smoke.sh; then
  echo "✗ 隔离环境冒烟失败，本次镜像不予上线，开始自动回滚"
  ./rollback.sh
  exit 1
fi

if [[ $DO_TLS -eq 1 ]]; then
  # TLS 分支只验握手，不注册：
  #   注册要有写库副作用，适合隔离栈；到这里只需确认"证书有效 + 网关愿意转发"
  #   以及反向用例"无客户端证书必须被拦"，详见该脚本头部注释。
  echo "== TLS 网关验证（仅握手 + 反向鉴权，不注册不写库）=="
  python3 tls_handshake_check.py --host 127.0.0.1 --port "${TLS_PORT:-8443}" \
      --ca   certs/ca.crt \
      --cert certs/client.crt --key certs/client.key || {
    echo "✗ TLS 正/反向握手验证未通过，检查 nginx 日志："; docker compose logs tls-gateway --tail=30
    exit 1
  }
  # 反向用例：不带客户端证书必须被网关拒绝
  python3 tls_handshake_check.py --host 127.0.0.1 --port "${TLS_PORT:-8443}" \
      --ca certs/ca.crt --expect-fail || {
    echo "✗ mTLS 鉴权失效：无客户端证书也能通过网关"
    exit 1
  }
fi

echo
echo "✓ 部署完成"
echo "  当前版本   : ${IMAGE_TAG}"
echo "  回滚锚点   : $(cat .last_good_tag 2>/dev/null || echo '无')"
echo "  明文业务端口 : 127.0.0.1:${SERVER_PORT:-8000}（不出主机）"
[[ $DO_TLS -eq 1 ]] && echo "  对外 TLS 端口 : 0.0.0.0:${TLS_PORT:-8443}（需客户端证书）"
echo "  客户端 config.ini 应写： ip=${CLIENT_SERVER_IP}"
docker compose "${COMPOSE_FILES[@]}" ps
