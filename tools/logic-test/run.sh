#!/bin/sh
# 轮播/抢占逻辑的确定性测试。不需要硬件，不需要 ESP-IDF。
#   ./tools/logic-test/run.sh
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
clang -O1 -Wall -I "$ROOT/firmware/main" -I "$ROOT/tools/logic-test/stub" \
      -o /tmp/clawd_pager_test \
      "$ROOT/tools/logic-test/pager_test.c" \
      "$ROOT/firmware/main/model/sessions.c" \
      "$ROOT/firmware/main/ui/pager.c" -lm
/tmp/clawd_pager_test
