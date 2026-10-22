#!/usr/bin/env bash
# ============================================================
# 数据库迁移器 —— 版本号表 + 按顺序应用 migrations/NNN_*.sql
#
# 用法：
#   ./migrate.sh --plan            # 只列待应用清单，不落任何变更（变更前必跑）
#   ./migrate.sh                   # 应用所有待执行的迁移
#   ./migrate.sh --check           # 只做漂移体检（基线与已应用迁移的 md5）
#   MIGRATE_MODE=local ./migrate.sh --plan    # 裸机模式（用本机 mysql 客户端）
#   MIGRATE_MODE=docker MYSQL_CONTAINER=smoke-mysql ./migrate.sh --plan
#
# ------------------------------------------------------------
# 三个必须知道的约束（每一条都对应 MySQL 的一个真实行为）
# ------------------------------------------------------------
# 1) DDL 在 MySQL 里【隐式提交】，跑一半失败无法自动回滚。
#    所以：迁移必须写成幂等（重复执行不报错），失败后由人工按文件内容
#    反向写一条新迁移，而不是指望脚本帮你 undo。
#
# 2) 全新容器走 docker-entrypoint-initdb.d 里的 schema.sql，
#    已有库走本脚本的 migrations/NNN_*.sql —— 两条路径的内容必须一致，
#    否则"新装的库"和"升级上来的库"会结构分叉。所以用 md5 硬卡基线一致性。
#
# 3) 已应用的迁移内容不允许事后修改：改了就等于两台机器的同一版本号
#    实际结构不同。发现 md5 漂移直接报错，不自动放行。
# ============================================================
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"

[[ -f .env ]] && { set -a; source .env; set +a; }

DB_NAME="${DB_NAME:-vcc}"
MYSQL_ROOT_PWD="${MYSQL_ROOT_PWD:-}"
MODE="${MIGRATE_MODE:-docker}"              # docker | local
MYSQL_CONTAINER="${MYSQL_CONTAINER:-vcc-mysql}"
BASELINE="$HERE/schema.sql"
MIG_DIR="$HERE/migrations"
TABLE="schema_migrations"

PLAN=0
CHECK=0
for a in "$@"; do
  case "$a" in
    --plan)  PLAN=1 ;;
    --check) CHECK=1 ;;
    --allow-drift) ALLOW_DRIFT=1 ;;
    *) echo "未知参数: $a"; exit 2 ;;
  esac
done
ALLOW_DRIFT="${ALLOW_DRIFT:-0}"

md5_of() { md5sum "$1" 2>/dev/null | awk '{print $1}'; }

# ---------- 连接通道 ----------
# 【修复-P1：未选择数据库】q()/run_file() 补 -D "$DB_NAME"
# 修改原因: 原命令没有 -D/--database，mysql 登录后处于"未选库"状态：
#           1) 执行 SQL 文件/CREATE TABLE/INSERT 直接报 "No database selected"，
#              迁移完全无法运行；
#           2) information_schema 查询里 table_schema=database() 求值为 NULL，
#              --plan/--check 模式 HAS_TABLE 恒为 0，误报"版本表不存在"。
# 修改效果: 所有连接默认选中 ${DB_NAME}，查询/建表/执行迁移全部落到正确的库，
#           plan/check 模式的探测也恢复真实语义。其余逻辑不变。
# 原代码:
#   q():        mysql -uroot -p"$MYSQL_ROOT_PWD" -N -B -e "$1"
#   run_file(): mysql -uroot -p"$MYSQL_ROOT_PWD" < "$1"
q() {   # 只查不写，返回纯结果
  #【P2修复：密码经命令行泄漏】-p"$PWD" 出现在进程参数里，ps/proc 可见。
  # 修改效果: 改经环境变量 MYSQL_PWD 传递（不进参数表、不进 shell 历史）；
  #           bare 模式对该条命令单独注入，docker 模式用 exec -e 注入。
  # 原代码保留备查：
  #   docker exec -i "$MYSQL_CONTAINER" mysql -uroot -p"$MYSQL_ROOT_PWD" -D "$DB_NAME" -N -B -e "$1"
  #   mysql -uroot -p"$MYSQL_ROOT_PWD" -D "$DB_NAME" -N -B -e "$1"
  if [[ "$MODE" == "docker" ]]; then
    docker exec -i -e MYSQL_PWD="$MYSQL_ROOT_PWD" "$MYSQL_CONTAINER" mysql -uroot -D "$DB_NAME" -N -B -e "$1"
  else
    MYSQL_PWD="$MYSQL_ROOT_PWD" mysql -uroot -D "$DB_NAME" -N -B -e "$1"
  fi
}
run_file() {   # 执行一个 SQL 文件
  #【P2修复：密码经命令行泄漏】同 q()
  if [[ "$MODE" == "docker" ]]; then
    docker exec -i -e MYSQL_PWD="$MYSQL_ROOT_PWD" "$MYSQL_CONTAINER" mysql -uroot -D "$DB_NAME" < "$1"
  else
    MYSQL_PWD="$MYSQL_ROOT_PWD" mysql -uroot -D "$DB_NAME" < "$1"
  fi
}

# ---------- 0. 基线一致性体检 ----------
BASE_MD5="$(md5_of "$BASELINE")"
FIRST_MIG="$(ls "$MIG_DIR" | sort | head -1)"
FIRST_MD5="$(md5_of "$MIG_DIR/$FIRST_MIG")"
echo "== 基线一致性 =="
echo "   schema.sql          : ${BASE_MD5}"
echo "   migrations/${FIRST_MIG} : ${FIRST_MD5}"
if [[ "$BASE_MD5" != "$FIRST_MD5" ]]; then
  echo "✗ 两条部署路径内容已分叉（全新容器 vs 存量升级）"
  echo "  修复： cp schema.sql migrations/${FIRST_MIG}   或反向 cp"
  [[ $ALLOW_DRIFT -eq 1 ]] || exit 3
  echo "   ⚠ --allow-drift 已指定，继续（记得尽快收敛）"
else
  echo "   ✓ 一致"
fi

# ---------- 1. 版本表 ----------
# --plan / --check 是"只看不改"的：连建版本表都不允许发生。
#   否则在一个还没跑过迁移的库上"看一眼"就会留下副作用，
#   这类"谎称只读"的工具最容易被随手在生产上跑。
if [[ $PLAN -eq 1 || $CHECK -eq 1 ]]; then
  HAS_TABLE="$(q "select count(*) from information_schema.tables
                  where table_schema=database() and table_name='${TABLE}';" 2>/dev/null | tail -1)"
  if [[ "$HAS_TABLE" == "1" ]]; then
    APPLIED="$(q "select version from \`${TABLE}\` order by version;")"
  else
    APPLIED=""
  fi
else
  q "CREATE TABLE IF NOT EXISTS \`${TABLE}\` (
       version    VARCHAR(64)  NOT NULL PRIMARY KEY,
       name       VARCHAR(255) NOT NULL,
       md5        CHAR(32)     NOT NULL,
       applied_at TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP,
       applied_by VARCHAR(64)  NOT NULL DEFAULT ''
     ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;" >/dev/null
  APPLIED="$(q "select version from \`${TABLE}\` order by version;")"
fi

PENDING=()
DRIFT=0
#【P2修复：for 拆词】原 `for f in $(ls ...)` 按空白拆词，文件名含空格即被拆成两项。
# 修改效果: 改为 NUL 分隔的安全遍历，文件名含空格/换行都不会拆错。
# 原代码保留备查：
# for f in $(ls "$MIG_DIR" | sort); do
while IFS= read -r -d '' f; do
  [[ "$f" == *.sql ]] || continue
  ver="${f%%_*}"
  md5="$(md5_of "$MIG_DIR/$f")"
  if echo "$APPLIED" | grep -qx "$ver"; then
    old="$(q "select md5 from \`${TABLE}\` where version='${ver}';")"
    if [[ "$old" != "$md5" ]]; then
      echo "✗ 迁移 ${f} 内容已被改动（登记 ${old:0:8}… / 现 ${md5:0:8}…）"
      DRIFT=1
    fi
  else
    PENDING+=("${f}|${ver}|${md5}")
  fi
#【P2修复】循环终止改为 NUL 分隔输入（配合上方 while read -d ''，原为 for $(ls) 拆词）
done < <(find "$MIG_DIR" -maxdepth 1 -type f -printf '%f\0' | sort -z)

echo
echo "== 待应用迁移 =="
if [[ ${#PENDING[@]} -eq 0 ]]; then
  echo "   （无，数据库已是最新）"
else
  for p in "${PENDING[@]}"; do
    echo "   ${p%%|*}  ($(echo "$p" | cut -d'|' -f3 | cut -c1-8))"
  done
fi

if [[ $DRIFT -eq 1 ]]; then
  echo
  echo "✗ 存在内容漂移，拒绝继续。若确认无害请用 --allow-drift，或新增一条修正迁移。"
  exit 4
fi

if [[ $CHECK -eq 1 || $PLAN -eq 1 ]]; then
  echo
  echo "✓ 体检完成（未做任何变更）。执行： ./migrate.sh"
  exit 0
fi

# ---------- 2. 应用 ----------
# hostname 在部分精简 shell 里不存在，容错一下（审计字段，缺了也不该让迁移失败）
HOST="$(hostname 2>/dev/null || echo unknown-host)"
WHO="${USER:-$(id -un 2>/dev/null || echo unknown)}@${HOST}"
for p in "${PENDING[@]}"; do
  f="$(echo "$p" | cut -d'|' -f1)"
  ver="$(echo "$p" | cut -d'|' -f2)"
  md5="$(echo "$p" | cut -d'|' -f3)"
  echo
  echo "== 应用 ${f} =="
  # 先执行再登记：即使 DDL 部分成功也会记下版本号，
  # 因为 MySQL 已经隐式提交了，记不记都不会更糟，
  # 而"没记录"会让下次重复执行同一条 —— 那才是灾难。
  if ! run_file "$MIG_DIR/$f"; then
    echo "✗ ${f} 执行失败。MySQL DDL 不可回滚，请人工核查后重试（脚本幂等，可直接再跑）"
    exit 5
  fi
  q "INSERT INTO \`${TABLE}\` (version,name,md5,applied_by) VALUES ('${ver}','${f}','${md5}','${WHO}')
     ON DUPLICATE KEY UPDATE md5=VALUES(md5), applied_at=CURRENT_TIMESTAMP, applied_by=VALUES(applied_by);" >/dev/null
  echo "   ✓ 已登记版本 ${ver}"
done

echo
echo "✓ 迁移完成，当前版本： $(q "select max(version) from \`${TABLE}\`;")"
