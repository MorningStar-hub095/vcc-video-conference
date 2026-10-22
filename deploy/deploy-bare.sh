#!/usr/bin/env bash
# ============================================================
# 方案 B：裸机部署（Ubuntu 22.04 主机 / 虚拟机，不用 Docker）
# 适用：已有的 192.168.1.100 虚拟机这类环境
# 用法： sudo ./deploy-bare.sh
# ============================================================
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
SRC="$ROOT/server"

INSTALL_DIR=/opt/vcc
SERVICE_NAME=playerhallserver
PORT="${SERVER_PORT:-8000}"

[[ -f "$HERE/.env" ]] && { set -a; source "$HERE/.env"; set +a; }
DB_NAME="${DB_NAME:-vcc}"; DB_USER="${DB_USER:-vcc}"
DB_PWD="${DB_PWD:?未设置 DB_PWD}"; DB_IP="${DB_IP:-127.0.0.1}"
MYSQL_ROOT_PWD="${MYSQL_ROOT_PWD:?未设置 MYSQL_ROOT_PWD}"

[[ $EUID -eq 0 ]] || { echo "请用 sudo 运行"; exit 1; }

echo "== 1/6 安装依赖 =="
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y g++ make libmysqlclient-dev libcrypt-dev mysql-server netcat-openbsd

echo "== 2/6 初始化数据库 =="
systemctl enable --now mysql
mysql -uroot -p"${MYSQL_ROOT_PWD}" <<SQL
CREATE DATABASE IF NOT EXISTS ${DB_NAME} DEFAULT CHARACTER SET utf8mb4;
CREATE USER IF NOT EXISTS '${DB_USER}'@'%' IDENTIFIED BY '${DB_PWD}';
GRANT ALL PRIVILEGES ON ${DB_NAME}.* TO '${DB_USER}'@'%';
FLUSH PRIVILEGES;
SQL
mysql -uroot -p"${MYSQL_ROOT_PWD}" < "$HERE/schema.sql"
echo "✓ 数据库就绪"

echo "== 3/6 备份旧版本 =="
if [[ -x "$INSTALL_DIR/playerhallserver" ]]; then
  TS="$(date +%Y%m%d-%H%M%S)"
  cp "$INSTALL_DIR/playerhallserver" "$INSTALL_DIR/playerhallserver.bak-$TS"
  echo "✓ 旧二进制已备份为 playerhallserver.bak-$TS"
fi

echo "== 4/6 编译（含 -lcrypt，bcrypt 依赖） =="
BUILD="$(mktemp -d)"
cp -r "$SRC/include" "$BUILD/include"
cp "$SRC"/src/*.cpp "$BUILD/src/"
rm -f "$BUILD/src/makefile" "$BUILD/src/"*.hbiFEK
# P0-2：源码 packdef.h 已改为运行期 _db_env() 读取环境变量，
# 不再用 sed 把明文口令写进二进制；口令改由 /opt/vcc/server.env 提供。
grep -q '_db_env' "$BUILD/include/packdef.h" \
  || { echo "✗ packdef.h 未做环境变量改造，请先执行 python3 _patch_db_env.py"; exit 1; }
g++ -std=c++11 -O2 -Wall -I"$BUILD/include" \
    "$BUILD"/src/*.cpp -o "$BUILD/playerhallserver" \
    -lpthread -lmysqlclient -lcrypt
echo "✓ 编译完成"

echo "== 5/6 安装 =="
mkdir -p "$INSTALL_DIR"
install -m 0755 "$BUILD/playerhallserver" "$INSTALL_DIR/playerhallserver"
id -u vcc >/dev/null 2>&1 || useradd -r -s /bin/false vcc

# 运行期密钥文件（600，仅 vcc 可读），systemd 通过 EnvironmentFile 注入
umask 077
cat > "$INSTALL_DIR/server.env" <<EOF
DB_IP=${DB_IP}
DB_NAME=${DB_NAME}
DB_USER=${DB_USER}
DB_PWD=${DB_PWD}
EOF
chmod 0600 "$INSTALL_DIR/server.env"
chown -R vcc:vcc "$INSTALL_DIR"
install -m 0644 "$HERE/playerhallserver.service" /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now "$SERVICE_NAME"

echo "== 6/6 放通防火墙 =="
ufw allow "${PORT}"/tcp comment 'VCC server' 2>/dev/null || true

echo
echo "== 验证 =="
sleep 2
#【P2修复：systemctl SIGPIPE】status 输出被 head 截断时 systemctl 收到 SIGPIPE 以 141 退出，
# set -e（若含 pipefail）会把脚本一起带崩。修改效果: 管道失败不中断部署。
# 原代码保留备查：
#   systemctl status "$SERVICE_NAME" --no-pager | head -12
systemctl status "$SERVICE_NAME" --no-pager | head -12 || true
nc -z 127.0.0.1 "$PORT" && echo "✓ 端口 ${PORT} 已监听" || { echo "✗ 端口未监听"; exit 1; }
python3 "$HERE/verify.py" --host 127.0.0.1 --port "$PORT"

echo
#【P2修复：bak 通配符提示错误】原提示 `cp ...bak-* .../playerhallserver`：
# 修改原因: bak 目录下通常有多份备份，通配符展开成多个源文件时 cp 会把最后一个
#           覆盖写到目标（或直接报错），回滚到哪个版本完全靠运气。
# 修改效果: 提示改为「取最近一份」的确定命令。
# 原代码保留备查：
#   echo "✓ 裸机部署完成。回滚： cp ${INSTALL_DIR}/playerhallserver.bak-* ${INSTALL_DIR}/playerhallserver && systemctl restart ${SERVICE_NAME}"
echo "✓ 裸机部署完成。回滚： cp \$(ls -t ${INSTALL_DIR}/playerhallserver.bak-* | head -1) ${INSTALL_DIR}/playerhallserver && systemctl restart ${SERVICE_NAME}"
