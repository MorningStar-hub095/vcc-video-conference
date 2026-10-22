#!/usr/bin/env bash
# ============================================================
# 冒烟隔离环境 —— 起 → 验 → 拆
#
# 用法：
#   ./smoke.sh              # 起一次性环境 → 跑 verify.py → 拆干净
#   ./smoke.sh --keep       # 失败时保留容器现场便于排查（需手动清理）
#   SMOKE_PORT=18001 ./smoke.sh
#
# 为什么必须隔离而不是连生产库：
#   verify.py 每次运行都会向 t_user 插入一条用户，而服务端没有注销协议。
#   verifier 生成的号码是 139 开头（verify.py:112），若用
#   `DELETE ... WHERE tel LIKE '139%'` 清理会误删真实用户（139 是中国移动号段）。
#   隔离环境碰不到生产卷，从根上消除这个风险。
#
# 为什么 project 必须写成 -p vcc-smoke（而不是在主 compose 里另起一套）：
#   卷与网络随 project name 自动隔离（变成 vcc-smoke_mysql_data），
#   每次都是空库初始化 —— 顺带验证了 schema.sql 的完整性，
#   这是连共享库跑永远覆盖不到的「首次部署」路径。
# ============================================================
set -uo pipefail

cd "$(dirname "$0")"

PROJECT="vcc-smoke"
# 读取部署配置：smoke 要验的是【这一版将被部署的镜像】，
# 若不看 .env 就会退回 latest，验出一个过期的旧镜像，等于没验。
[[ -f .env ]] && { set -a; source .env; set +a; }
PORT="${SMOKE_PORT:-18000}"
KEEP=0
BUILD=()
for arg in "$@"; do
  case "$arg" in
    --keep)  KEEP=1 ;;
    --build) BUILD=(--build) ;;
  esac
done
# deploy.sh 会把自己的 IMAGE_TAG export 出来，这里必须原样继承：
#   否则 deploy.sh 用 SHA 标签部署，smoke 却在验一个 latest 旧镜像，验了等于没验。
: "${IMAGE_TAG:=latest}"
export IMAGE_TAG

# 关键：overlay 里 server 的 ports 与主文件【逐字符相同】，靠这里的 SERVER_PORT
# 决定最终发布到哪个宿主端口。两条完全相同的规则会被 compose 去重成一条，
# 从而不必改动主 compose 就能把 smoke 挪开、避免与生产栈抢 8000。
export SERVER_PORT="$PORT"

# Git Bash 下 docker 不在默认 PATH，补齐常见位置
case ":$PATH:" in
  *":/c/Program Files/Docker/Docker/resources/bin:"*) ;;
  *) PATH="$PATH:/c/Program Files/Docker/Docker/resources/bin" ;;
esac
export MSYS_NO_PATHCONV=1

cleanup() {
  # 失败也一定要拆：否则残留容器会占住 18000 端口，下次直接起不来
  if [[ $KEEP -eq 1 ]]; then
    echo
    echo "!! --keep 已指定，保留现场。手动清理："
    echo "!!   docker compose -p $PROJECT down -v"
    return
  fi
  docker compose -p "$PROJECT" down -v >/dev/null 2>&1
}
trap cleanup EXIT

COMPOSE_FILES=(-f docker-compose.yml -f docker-compose.smoke.yml)

echo "== 0/3 预检 =="
if ! docker version --format '{{.Server.Version}}' >/dev/null 2>&1; then
  echo "✗ Docker 引擎不可用，请先启动 Docker Desktop"
  exit 2
fi
docker compose "${COMPOSE_FILES[@]}" config --quiet || {
  echo "✗ compose 配置校验失败，请检查 docker-compose.smoke.yml"
  exit 2
}
echo "   Docker 就绪，compose 配置通过"

echo
echo "== 1/3 起隔离环境 (project=$PROJECT, 宿主端口=$PORT) =="
# --wait 会等到 healthcheck 转 healthy；依赖 Dockerfile 里的 health-probe
#【P2修复：bash<4.4 空数组 unbound】未传 --build 时 BUILD=() 为空数组，
# bash 4.3 及以下在 set -u 下展开 "${BUILD[@]}" 会报 unbound variable 直接退出。
# 修改效果: 用 ${BUILD[@]+...} 保护性展开——数组为空时不展开任何词；非空时行为不变。
# 原代码保留备查：
#     up -d --wait "${BUILD[@]}"; then
if ! docker compose -p "$PROJECT" "${COMPOSE_FILES[@]}" \
      up -d --wait ${BUILD[@]+"${BUILD[@]}"}; then
  echo "✗ 环境启动失败，容器日志："
  docker compose -p "$PROJECT" logs --tail=40
  exit 1
fi
echo "   已就绪"

echo
echo "== 2/3 冒烟 =="
python3 verify.py --host 127.0.0.1 --port "$PORT"
RC=$?

echo
echo "== 3/3 结论 =="
if [[ $RC -eq 0 ]]; then
  echo "✓ 冒烟通过（隔离环境已自动销毁，生产库未受影响）"
else
  echo "✗ 冒烟失败 rc=$RC"
fi
exit $RC
