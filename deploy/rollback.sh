#!/usr/bin/env bash
# ============================================================
# 回滚：把服务端切回上一个可用镜像标签，并可选还原数据库
# 用法：
#   ./rollback.sh              # 回滚服务端到 .last_good_tag
#   ./rollback.sh --db backups/db-20261013-140000.sql   # 同时还原数据库
# ============================================================
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"

[[ -f .env ]] || { echo "✗ 缺少 .env"; exit 1; }
set -a; source .env; set +a

DB_FILE=""
[[ "${1:-}" == "--db" ]] && DB_FILE="${2:-}"

# ---------- 1. 确定目标标签 ----------
TARGET="$(cat .last_good_tag 2>/dev/null || true)"
if [[ -z "$TARGET" ]]; then
  echo "⚠ 未找到 .last_good_tag，回退到 latest"
  TARGET="latest"
fi

# 防线：回滚目标与当前标签相同 = 回滚到自己，属于空操作。
# 早期 .last_good_tag 被错误写成新标签时，这种情况会静默"成功"，极具误导性。
CUR="$(cat .current_tag 2>/dev/null || echo '')"
if [[ -n "$CUR" && "$TARGET" == "$CUR" ]]; then
  echo "✗ 回滚目标与当前标签相同（${CUR}），拒绝空回滚"
  echo
  echo "  可用的回滚目标是另一个镜像标签，当前镜像列表："
  docker images --format 'vcc-server:{{.Tag}}' 2>/dev/null | grep -v '<none>' | sed 's/^/    /'
  echo
  echo "  手动指定： IMAGE_TAG=<目标标签> docker compose up -d server"
  exit 1
fi
echo "== 回滚目标镜像： vcc-server:${TARGET} =="

# ---------- 2. 停服 ----------
docker compose stop server

# ---------- 3. 还原数据库（可选） ----------
if [[ -n "$DB_FILE" ]]; then
  [[ -f "$DB_FILE" ]] || { echo "✗ 备份文件不存在: $DB_FILE"; exit 1; }
  echo "== 还原数据库 $DB_FILE =="
  docker compose start mysql
  docker exec -i vcc-mysql mysql -uroot -p"${MYSQL_ROOT_PWD}" < "$DB_FILE"
  echo "✓ 数据库已还原"
fi

# ---------- 4. 用旧标签重建容器 ----------
export IMAGE_TAG="$TARGET"
docker compose up -d server

# ---------- 5. 验证 ----------
echo "== 回滚后验证 =="
for i in $(seq 1 30); do
  docker inspect -f '{{.State.Health.Status}}' vcc-server 2>/dev/null | grep -q healthy && break
  sleep 2
done
python3 verify.py --host 127.0.0.1 --port "${SERVER_PORT:-8000}" \
  && echo "✓ 回滚成功" \
  || { echo "✗ 回滚后仍异常，请查看： docker compose logs server"; exit 1; }

# 回滚成功：此刻真正运行的就是 TARGET，同步 current_tag。
# 注意 last_good_tag 保持不变——它仍指向 TARGET 的上一个版本，供二次回滚。
echo "${TARGET}" > ".current_tag"
echo "  当前版本   : ${TARGET}"
