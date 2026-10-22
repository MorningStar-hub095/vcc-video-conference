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
  #【P2修复：首次部署回滚到自身】
  # 修改原因: 原代码在 .last_good_tag 缺失（首次部署、锚点从未建立）时静默回退到
  #           "latest"——而 latest 往往就是刚部署的同一镜像，回滚等于原样重启，
  #           还让运维误以为"已回滚到旧版本"。
  # 修改效果: 无锚点即报错退出，并列出可用镜像让运维显式指定目标。
  # 原代码保留备查：
  #   echo "⚠ 未找到 .last_good_tag，回退到 latest"
  #   TARGET="latest"
  echo "✗ 未找到 .last_good_tag（首次部署或锚点未建立），没有可回滚的版本"
  echo
  echo "  当前镜像列表："
  docker images --format 'vcc-server:{{.Tag}}' 2>/dev/null | grep -v '<none>' | sed 's/^/    /'
  echo
  echo "  显式指定目标： IMAGE_TAG=<目标标签> docker compose up -d server"
  exit 1
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
  #【P2修复：密码经命令行泄漏】同 migrate.sh：-p"$PWD" 对 ps 可见，改经 MYSQL_PWD 环境变量
  # 原代码保留备查：
  #   docker exec -i vcc-mysql mysql -uroot -p"${MYSQL_ROOT_PWD}" < "$DB_FILE"
  docker exec -i -e MYSQL_PWD="${MYSQL_ROOT_PWD}" vcc-mysql mysql -uroot < "$DB_FILE"
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
# 【修复-P1：回滚验证向生产库写数据】
# 修改原因: 原代码用 verify.py 做回滚验证，而 verify.py 第 3 步会真实注册一个新用户
#           写进生产 t_user 表——与 deploy.sh 自己声明的"verify.py 不得直连生产库"
#           相矛盾，每次回滚都往生产库灌一条垃圾账号。
# 修改效果: 改为两项【只读】探测：
#           1) 容器 healthcheck 必须达到 healthy（未达即失败退出）；
#           2) 对服务端口做一次 TCP 连接级探测（连接后立即断开，不发送任何
#              协议包、不写库）。验证语义仍覆盖"进程活着 + 端口可达"，
#              但对生产数据零副作用。完整功能验证由运维在测试环境跑 verify.py。
# 原代码:
#   python3 verify.py --host 127.0.0.1 --port "${SERVER_PORT:-8000}" \
#     && echo "✓ 回滚成功" \
#     || { echo "✗ 回滚后仍异常，请查看： docker compose logs server"; exit 1; }
docker inspect -f '{{.State.Health.Status}}' vcc-server 2>/dev/null | grep -q healthy \
  || { echo "✗ 回滚后容器未恢复 healthy，请查看： docker compose logs server"; exit 1; }
if timeout 3 bash -c "</dev/tcp/127.0.0.1/${SERVER_PORT:-8000}" 2>/dev/null; then
  echo "✓ 回滚成功（容器 healthy + 端口可连，未触碰生产数据）"
else
  echo "✗ 端口 ${SERVER_PORT:-8000} 不可连，请查看： docker compose logs server"; exit 1
fi

# 回滚成功：此刻真正运行的就是 TARGET，同步 current_tag。
# 注意 last_good_tag 保持不变——它仍指向 TARGET 的上一个版本，供二次回滚。
echo "${TARGET}" > ".current_tag"
echo "  当前版本   : ${TARGET}"
