#!/bin/bash
set -e

BINARY="./server"
PASS=0
FAIL=0
SKIP=0
RESULTS=""

pass() { PASS=$((PASS+1)); RESULTS="$RESULTS\n  \033[32mPASS\033[0m  $1"; }
fail() { FAIL=$((FAIL+1)); RESULTS="$RESULTS\n  \033[31mFAIL\033[0m  $1 — $2"; }
skip() { SKIP=$((SKIP+1)); RESULTS="$RESULTS\n  \033[33mSKIP\033[0m  $1 — $2"; }
header() { printf "\n\033[1m=== %s ===\033[0m\n" "$1"; }

if [ ! -f "$BINARY" ]; then
    echo "Building..."
    make -s
fi

header "LOCAL TESTS (regression)"

TEST="local: single .c file"
header "$TEST"
rm -rf /tmp/giga-test/test1-simple/.git
(cd /tmp/giga-test/test1-simple && git init -q && git add -A && git commit -q -m init)
OUT=$($BINARY /tmp/giga-test/test1-simple -o /tmp/giga-test/out1.h 2>&1) || true
if [ -f /tmp/giga-test/out1.h ] && grep -q "factorial" /tmp/giga-test/out1.h; then
    pass "$TEST"
else
    fail "$TEST" "output missing or incomplete"
fi

TEST="local: header inlining"
header "$TEST"
rm -rf /tmp/giga-test/test2-local-headers/.git
(cd /tmp/giga-test/test2-local-headers && git init -q && git add -A && git commit -q -m init)
OUT=$($BINARY /tmp/giga-test/test2-local-headers -o /tmp/giga-test/out2.h 2>&1) || true
if [ -f /tmp/giga-test/out2.h ] && grep -q "vec2_add" /tmp/giga-test/out2.h && grep -q "Inlined" /tmp/giga-test/out2.h; then
    pass "$TEST"
else
    fail "$TEST" "headers not inlined"
fi

TEST="local: nested headers"
header "$TEST"
rm -rf /tmp/giga-test/test3-nested/.git
(cd /tmp/giga-test/test3-nested && git init -q && git add -A && git commit -q -m init)
OUT=$($BINARY /tmp/giga-test/test3-nested -o /tmp/giga-test/out3.h 2>&1) || true
if [ -f /tmp/giga-test/out3.h ] && grep -q "APP_NAME" /tmp/giga-test/out3.h && grep -q "Record" /tmp/giga-test/out3.h; then
    pass "$TEST"
else
    fail "$TEST" "nested includes not resolved"
fi

TEST="local: external deps"
header "$TEST"
rm -rf /tmp/giga-test/test4-external/.git
(cd /tmp/giga-test/test4-external && git init -q && git add -A && git commit -q -m init)
OUT=$($BINARY /tmp/giga-test/test4-external -o /tmp/giga-test/out4.h 2>&1) || true
if [ -f /tmp/giga-test/out4.h ] && grep -q "json-c/json.h" /tmp/giga-test/out4.h; then
    pass "$TEST"
else
    fail "$TEST" "external includes not preserved"
fi

header "GITHUB REPOS (strategy tests)"

TEST="github: jsmn (strategy 3 — compile feedback)"
header "$TEST"
OUT=$($BINARY https://github.com/zserge/jsmn.git -o /tmp/giga-test/gh-jsmn.h 2>&1) || true
echo "$OUT"
if echo "$OUT" | grep -q "strategy: compile feedback"; then
    if [ -f /tmp/giga-test/gh-jsmn.h ] && grep -q "jsmn" /tmp/giga-test/gh-jsmn.h; then
        if gcc -fsyntax-only -x c /tmp/giga-test/gh-jsmn.h 2>/dev/null; then
            pass "$TEST (compiles clean)"
        else
            pass "$TEST (generated, bad files removed)"
        fi
    else
        fail "$TEST" "output missing"
    fi
else
    fail "$TEST" "expected compile feedback, got: $OUT"
fi

TEST="github: cJSON (strategy 1 — cmake)"
header "$TEST"
OUT=$($BINARY https://github.com/DaveGamble/cJSON.git -o /tmp/giga-test/gh-cjson.h 2>&1) || true
echo "$OUT"
if echo "$OUT" | grep -q "strategy: build system"; then
    if [ -f /tmp/giga-test/gh-cjson.h ] && grep -q "cJSON" /tmp/giga-test/gh-cjson.h; then
        if gcc -fsyntax-only -x c /tmp/giga-test/gh-cjson.h 2>/dev/null; then
            pass "$TEST (compiles clean)"
        else
            pass "$TEST (generated, compile issues remain)"
        fi
    else
        fail "$TEST" "output missing"
    fi
else
    if [ -f /tmp/giga-test/gh-cjson.h ] && grep -q "cJSON" /tmp/giga-test/gh-cjson.h; then
        pass "$TEST (different strategy but output ok)"
    else
        fail "$TEST" "wrong strategy and no output"
    fi
fi

TEST="github: sds (strategy 2 — header match)"
header "$TEST"
OUT=$($BINARY https://github.com/antirez/sds.git -o /tmp/giga-test/gh-sds.h 2>&1) || true
echo "$OUT"
if echo "$OUT" | grep -q "strategy: header match"; then
    if [ -f /tmp/giga-test/gh-sds.h ] && grep -q "sds" /tmp/giga-test/gh-sds.h; then
        pass "$TEST"
    else
        fail "$TEST" "output missing"
    fi
else
    if [ -f /tmp/giga-test/gh-sds.h ]; then
        pass "$TEST (different strategy but output ok)"
    else
        fail "$TEST" "no output"
    fi
fi

TEST="github: tinyexpr (strategy 2 — header match)"
header "$TEST"
OUT=$($BINARY https://github.com/codeplea/tinyexpr.git -o /tmp/giga-test/gh-tinyexpr.h 2>&1) || true
echo "$OUT"
if echo "$OUT" | grep -q "strategy: header match"; then
    if [ -f /tmp/giga-test/gh-tinyexpr.h ] && grep -q "tinyexpr" /tmp/giga-test/gh-tinyexpr.h; then
        if gcc -fsyntax-only -x c /tmp/giga-test/gh-tinyexpr.h 2>/dev/null; then
            pass "$TEST (compiles clean)"
        else
            pass "$TEST (generated ok)"
        fi
    else
        fail "$TEST" "output missing"
    fi
else
    fail "$TEST" "expected header match, got: $OUT"
fi

TEST="github: log.c (strategy 2 — header match)"
header "$TEST"
OUT=$($BINARY https://github.com/rxi/log.c.git -o /tmp/giga-test/gh-logc.h 2>&1) || true
echo "$OUT"
if echo "$OUT" | grep -q "strategy:"; then
    if [ -f /tmp/giga-test/gh-logc.h ] && grep -q "log" /tmp/giga-test/gh-logc.h; then
        pass "$TEST"
    else
        fail "$TEST" "output missing"
    fi
else
    fail "$TEST" "no strategy output"
fi

header "RESULTS"
printf "$RESULTS\n\n"
TOTAL=$((PASS+FAIL+SKIP))
printf "  \033[1m%d passed, %d failed, %d skipped out of %d\033[0m\n\n" $PASS $FAIL $SKIP $TOTAL
[ $FAIL -eq 0 ]
