#!/usr/bin/env bash
# ============================================================
# 注入回归 + 数据库完整性检查 —— 起一次性隔离栈 → 打恶意包 → 查库 → 拆
#
# 用法：
#   ./security_smoke.sh                # 默认跑在 18002（独立 project vcc-sec）
#   SMOKE_PORT=18003 ./security_smoke.sh
#   ./security_smoke.sh --keep         # 失败保留现场
#
# 为什么必须隔离 + 为什么要查库：
#   A 组用例会真实往 t_user 写「带引号的昵称」，B 组会注册用户，
#   协议里没有删除接口，所以只能让整套数据随容器卷一起销毁。
#   另外仅看应用返回值是不够的 —— '); DROP TABLE t_user; -- 这种 payload
#   在某些写法下返回值照样正常，只有回库数行数、确认表还在，才算真闭环。
# ============================================================
set -uo pipefail

cd "$(dirname "$0")"

PROJECT="vcc-sec"
# 读取部署配置，保证验的是【即将上线的那个标签】，而不是缺省的 latest
[[ -f .env ]] && { set -a; source .env; set +a; }
PORT="${SMOKE_PORT:-18002}"
KEEP=0
[[ "${1:-}" == "--keep" ]] && KEEP=1
: "${IMAGE_TAG:=latest}"
export IMAGE_TAG
export SERVER_PORT="$PORT"
export MSYS_NO_PATHCONV=1

case ":$PATH:" in
  *":/c/Program Files/Docker/Docker/resources/bin:"*) ;;
  *) PATH="$PATH:/c/Program Files/Docker/Docker/resources/bin" ;;
esac

COMPOSE_FILES=(-f docker-compose.yml -f docker-compose.smoke.yml)

cleanup() {
  [[ $KEEP -eq 1 ]] && { echo "!! --keep: 手动清理 docker compose -p $PROJECT down -v"; return; }
  docker compose -p "$PROJECT" down -v >/dev/null 2>&1
}
trap cleanup EXIT

echo "== 1/4 起隔离栈 (project=$PROJECT, 端口=$PORT, 镜像=$IMAGE_TAG) =="
docker compose -p "$PROJECT" "${COMPOSE_FILES[@]}" up -d --wait || {
  echo "✗ 启动失败"; docker compose -p "$PROJECT" logs --tail=40; exit 1; }

# 起栈时的行数基线：用户只能由本脚本写入，所以 diff 出来全是自己的攻击数据
# 注意：.env 里的 MYSQL_ROOT_PWD 是【生产】口令，冒烟栈用的是
# docker-compose.smoke.yml 里写死的 smoke_root_only，两者必须区分开，
# 否则一不小心就拿着生产口令去连（连不上是运气好，连上了更糟）。
MYSQL_CONTAINER="smoke-mysql"
ROOT_PWD="${SMOKE_MYSQL_ROOT_PWD:-smoke_root_only}"
Q() { docker exec "$MYSQL_CONTAINER" mysql -uroot -p"$ROOT_PWD" -N -B -e "$1" 2>/dev/null; }

BEFORE="$(Q "select count(*) from vcc.t_user;")"
echo "   基线行数: ${BEFORE}"

echo "== 2/4 打恶意包 =="
python3 security_smoke.py --host 127.0.0.1 --port "$PORT"
RC=$?

echo "== 3/4 回库核对 =="
AFTER="$(Q  "select count(*) from vcc.t_user;")"
TABLE_OK="$(Q "select count(*) from information_schema.tables where table_schema='vcc' and table_name='t_user';")"
IDX_OK="$(Q   "select count(*) from information_schema.statistics where table_schema='vcc' and table_name='t_user';")"
echo "   攻击后行数: ${AFTER}   t_user 存在: ${TABLE_OK}   索引条数: ${IDX_OK}"

INTEGRITY=0
[[ "${TABLE_OK}" == "1" ]] || { echo "✗ t_user 不见了 —— 注入真的执行了！"; INTEGRITY=1; }
[[ "${AFTER}" -gt "${BEFORE}" ]] || { echo "✗ 行数未增长，流程可能没走通"; INTEGRITY=1; }
# 3 个索引都在 = schema.sql 的幂等建索引逻辑生效（幂等改动的自带验证）
[[ "${IDX_OK}" -ge 3 ]] || { echo "✗ 索引缺失（预期 >=3）"; INTEGRITY=1; }

echo "== 4/4 结论 =="
if [[ $RC -eq 0 && $INTEGRITY -eq 0 ]]; then
  echo "✓ 注入防住 + 业务无回归 + 库结构完好（隔离栈即将销毁）"
else
  echo "✗ rc=$RC integrity=$INTEGRITY"
fi
exit $(( RC + INTEGRITY ))
