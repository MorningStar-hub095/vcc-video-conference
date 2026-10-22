#!/usr/bin/env bash
# ============================================================
# 准备构建上下文
# 作用：把服务端源码拷贝到 deploy/.staging/PlayerHallServer/
#
# 为什么要拷贝：
#   1) 源目录里混有 .o / .pro.user / makefile 教学模板 等构建无关文件，
#      直接进入上下文会污染构建；
#   2) 统一在 .staging 生成一份可逐字节校验的构建上下文。
# ============================================================
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

# 唯一权威源码目录
SRC="$ROOT/server"
STAGE="$HERE/.staging"

if [[ ! -d "$SRC/src" ]]; then
  echo "✗ 未找到服务端源码：$SRC" >&2
  exit 1
fi

# 校验：逐字节比对 .staging 与权威源码。
#   构建上下文 (.staging) 若停留在旧版本，编译出来的镜像会"看起来一切正常"
#   却不含最新修复 —— 这是最难发现的一类事故，所以必须能被单独检查出来。
verify() {
  local bad=0 n=0
  for pair in "include:$STAGE/PlayerHallServer/include" "src:$STAGE/PlayerHallServer/src"; do
    sub="${pair%%:*}"; dst="${pair#*:}"
    for f in "$SRC/$sub"/*.h "$SRC/$sub"/*.cpp; do
      [[ -e "$f" ]] || continue
      b="$(basename "$f")"
      case "$b" in makefile|*.hbiFEK) continue ;; esac
      n=$((n + 1))
      if [[ ! -f "$dst/$b" ]]; then
        echo "✗ 构建上下文缺少：$sub/$b"; bad=1; continue
      fi
      cmp -s "$f" "$dst/$b" || { echo "✗ 内容已过期：$sub/$b"; bad=1; }
    done
  done
  echo "   比对文件数：$n"
  return $bad
}

# --check：只体检，不重新拷贝（CI 与 ci-local.sh 在构建前必跑一次）
if [[ "${1:-}" == "--check" ]]; then
  if verify; then
    echo "✓ 构建上下文与源码一致"
    exit 0
  fi
  echo "✗ 构建上下文已过期，请重跑 ./prepare.sh" >&2
  exit 1
fi

rm -rf "$STAGE"
mkdir -p "$STAGE/PlayerHallServer/include" "$STAGE/PlayerHallServer/src"

cp "$SRC"/include/*.h   "$STAGE/PlayerHallServer/include/"
cp "$SRC"/src/*.cpp     "$STAGE/PlayerHallServer/src/"

# 清理无关文件（根目录空桩 main.cpp、教学用 makefile、备份文件）
rm -f "$STAGE/PlayerHallServer/src/makefile"
rm -f "$STAGE/PlayerHallServer/src/"*.hbiFEK

echo "✓ 构建上下文已就绪：$STAGE"
echo "  头文件: $(ls "$STAGE/PlayerHallServer/include" | wc -l) 个"
echo "  源文件: $(ls "$STAGE/PlayerHallServer/src"     | wc -l) 个"

# 拷完立刻自检：拷贝静默失败（权限/编码/磁盘）同样会让"构建成功但内容旧"
verify || { echo "✗ 拷贝后自检未通过" >&2; exit 1; }
echo "✓ 拷贝后自检通过"
