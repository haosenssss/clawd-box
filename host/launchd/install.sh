#!/bin/sh
#
# 注册/注销开机自启的守护进程。
#
# plist 里的路径必须是绝对路径（launchd 不认 ~ 也不继承 shell 的 PATH），
# 所以模板里留了 __BUN__ / __REPO__ 两个占位符，在这里替换成本机真实路径。
# 直接 cp 模板会得到一个跑不起来的 plist。
set -e

LABEL=com.clawdbox.daemon
PLIST_DIR="$HOME/Library/LaunchAgents"
TARGET="$PLIST_DIR/$LABEL.plist"
REPO="$(cd "$(dirname "$0")/../.." && pwd)"

if [ "$1" = "--uninstall" ]; then
    launchctl unload "$TARGET" 2>/dev/null || true
    rm -f "$TARGET"
    echo "已注销 $LABEL"
    exit 0
fi

BUN="$(command -v bun || true)"
if [ -z "$BUN" ]; then
    echo "找不到 bun。先装：curl -fsSL https://bun.sh/install | bash" >&2
    exit 1
fi

mkdir -p "$PLIST_DIR"
sed -e "s|__BUN__|$BUN|g" -e "s|__REPO__|$REPO|g" \
    "$REPO/host/launchd/$LABEL.plist" > "$TARGET"

launchctl unload "$TARGET" 2>/dev/null || true
launchctl load "$TARGET"

echo "已注册 $LABEL"
echo "  bun : $BUN"
echo "  仓库: $REPO"
echo "  日志: /tmp/clawdbox-daemon.log"
