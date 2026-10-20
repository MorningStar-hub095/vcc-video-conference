#!/usr/bin/env bash
# ============================================================
# 本地一键质量门禁 —— 与 CI 跑【完全相同的三条检查】
#
# 用法：
#   ./ci-local.sh                 # 准备 → 构建 → 功能冒烟 → 注入回归
#   ./ci-local.sh --with-tls      # 额外验 TLS 网关握手（需 certs/ 就绪）
#   ./ci-local.sh --fast          # 跳过构建（已有镜像时）
#
# 为什么要有本地入口：
#   本仓库目前没有远端（git remote 为空），GitHub Actions 跑不起来。
#   但"门禁"这件事不能等有远端才做 —— 所以把门禁写成这个脚本，
#   CI workflow 直接调用它：本地和 CI 永远同一套判定，不会出现
#   "本地绿的、CI 红的"这种扯皮。
#
# fail fast 顺序（越便宜越靠前）：
#   1 源码同步与新鲜度 → 2 编译 → 3 功能冒烟 → 4 安全回归
# ============================================================
set -euo pipefail

cd "$(dirname "$0")"

DO_BUILD=1
DO_TLS=0
for a in "$@"; do
  case "$a" in
    --fast)      DO_BUILD=0 ;;
    --with-tls)  DO_TLS=1  ;;
  esac
done

export MSYS_NO_PATHCONV=1
PATH="$PATH:/c/Program Files/Docker/Docker/resources/bin"
export PATH

FAIL() { echo; echo "✗ 门禁未通过：$1"; exit 1; }

echo "== 1/4 源码同步与新鲜度 =="
./prepare.sh >/dev/null
# 构建上下文 (.staging) 必须是【当前源码】的拷贝：
#   它若停留在旧版本，编译出来的镜像会看起来"一切正常"却不含最新修复。
./prepare.sh --check || FAIL "构建上下文与源码不一致"

echo "== 2/4 构建镜像 =="
if [[ $DO_BUILD -eq 1 ]]; then
  docker compose build server || FAIL "镜像构建失败（看上面的编译错误）"
else
  echo "   --fast：跳过构建"
fi

echo "== 3/4 功能冒烟（隔离栈） =="
./smoke.sh || FAIL "功能冒烟失败"

echo "== 4/4 注入回归（隔离栈） =="
./security_smoke.sh || FAIL "注入回归失败"

if [[ $DO_TLS -eq 1 ]]; then
  echo "== 附加：TLS 网关握手 =="
  python3 tls_handshake_check.py --port "${TLS_PORT:-8443}" \
      --cert certs/client.crt --key certs/client.key || FAIL "TLS 握手验证失败"
  python3 tls_handshake_check.py --port "${TLS_PORT:-8443}" --expect-fail \
      || FAIL "mTLS 鉴权失效（无证书也能进）"
fi

echo
echo "✓ 全部门禁通过（编译 / 功能 / 安全）"
