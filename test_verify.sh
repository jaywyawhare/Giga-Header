#!/bin/bash

BINARY="$(cd "$(dirname "$0")" && pwd)/server"
W="/tmp/giga-verify"
PASS=0
FAIL=0
RESULTS=""

pass() { PASS=$((PASS+1)); RESULTS="$RESULTS\n  \033[32mPASS\033[0m  $1"; }
fail() { FAIL=$((FAIL+1)); RESULTS="$RESULTS\n  \033[31mFAIL\033[0m  $1 — $2"; }
header() { printf "\n\033[1m=== %s ===\033[0m\n" "$1"; }

for d in "$W"/*-run; do [ -d "$d" ] && rm -rf "$d"; done
rm -f "$W"/test_*.c
mkdir -p "$W"

header "Cloning repos"
clone() {
    local url="$1" dest="$2"
    [ -d "$dest/.git" ] && return 0
    for i in 1 2 3; do
        git clone --depth 1 "$url" "$dest" 2>/dev/null && return 0
        sleep 2
    done
    echo "  WARN: failed to clone $url"
    return 1
}
clone https://github.com/zserge/jsmn.git      "$W/jsmn"
clone https://github.com/antirez/sds.git       "$W/sds"
clone https://github.com/codeplea/tinyexpr.git "$W/tinyexpr"
clone https://github.com/rxi/log.c.git         "$W/logc"
clone https://github.com/DaveGamble/cJSON.git  "$W/cJSON"
echo "  done"

run_test() {
    local name="$1"
    local repo_path="$2"
    local normal_compile="$3"
    local test_src="$4"
    local include_old="$5"
    local include_new="$6"
    local extra_sed="$7"
    local link_flags="$8"

    TEST="$name"
    header "$TEST"

    local d="$W/$name-run"
    mkdir -p "$d"
    cp "$W/test_${name}.c" "$d/test.c"

    echo "  [normal build]"
    if ! (cd "$d" && eval "$normal_compile" 2>&1); then
        fail "$TEST" "normal compile failed"
        return
    fi
    (cd "$d" && ./normal > out_normal.txt)

    echo "  [generating header-only]"
    $BINARY "$repo_path" -o "$d/combined.h" 2>&1 | grep "strategy:"

    echo "  [header-only build]"
    if [ -n "$extra_sed" ]; then
        sed "$extra_sed;s|$include_old|$include_new|" "$d/test.c" > "$d/test_ho.c"
    else
        sed "s|$include_old|$include_new|" "$d/test.c" > "$d/test_ho.c"
    fi

    if ! (cd "$d" && gcc -o ho test_ho.c $link_flags 2>&1); then
        fail "$TEST" "header-only compile failed"
        return
    fi
    (cd "$d" && ./ho > out_ho.txt)

    echo "  normal:      $(tr '\n' ' ' < "$d/out_normal.txt")"
    echo "  header-only: $(tr '\n' ' ' < "$d/out_ho.txt")"
    if diff -q "$d/out_normal.txt" "$d/out_ho.txt" > /dev/null 2>&1; then
        pass "$TEST"
    else
        fail "$TEST" "output differs"
        diff "$d/out_normal.txt" "$d/out_ho.txt"
    fi
}

cat > "$W/test_jsmn.c" << 'EOF'
#include <stdio.h>
#include <string.h>
#define JSMN_STATIC
#include "jsmn.h"

int main(void) {
    const char *json = "{\"name\":\"test\",\"value\":42}";
    jsmn_parser parser;
    jsmntok_t tokens[16];
    jsmn_init(&parser);
    int r = jsmn_parse(&parser, json, strlen(json), tokens, 16);
    printf("tokens: %d\n", r);
    printf("type[0]: %d\n", tokens[0].type);
    printf("type[1]: %d\n", tokens[1].type);
    return 0;
}
EOF

cat > "$W/test_sds.c" << 'EOF'
#include <stdio.h>
#include "sds.h"

int main(void) {
    sds s = sdsnew("Hello");
    s = sdscat(s, " World");
    printf("str: %s\n", s);
    printf("len: %zu\n", sdslen(s));
    sds s2 = sdscatprintf(sdsempty(), "%d+%d=%d", 1, 2, 3);
    printf("fmt: %s\n", s2);
    sdsfree(s);
    sdsfree(s2);
    return 0;
}
EOF

cat > "$W/test_tinyexpr.c" << 'EOF'
#include <stdio.h>
#include "tinyexpr.h"

int main(void) {
    double r1 = te_interp("2+3*4", 0);
    printf("2+3*4 = %.1f\n", r1);
    double r2 = te_interp("sqrt(9)", 0);
    printf("sqrt(9) = %.1f\n", r2);
    double r3 = te_interp("(5+3)/2", 0);
    printf("(5+3)/2 = %.1f\n", r3);
    return 0;
}
EOF

cat > "$W/test_logc.c" << 'EOF'
#include <stdio.h>
#include <string.h>
#include "log.h"

static char buf[4096];
static int bpos = 0;

static void cb(log_Event *ev) {
    bpos += snprintf(buf + bpos, sizeof(buf) - (size_t)bpos,
        "level=%d msg=%s\n", ev->level, (const char *)ev->fmt);
}

int main(void) {
    log_set_quiet(1);
    log_add_callback(cb, NULL, LOG_TRACE);
    log_trace("trace_msg");
    log_info("info_msg");
    log_warn("warn_msg");
    log_error("error_msg");
    printf("%s", buf);
    return 0;
}
EOF

cat > "$W/test_cjson.c" << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include "cJSON.h"

int main(void) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", "giga");
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddTrueToObject(root, "working");
    cJSON *arr = cJSON_AddArrayToObject(root, "items");
    cJSON_AddItemToArray(arr, cJSON_CreateNumber(10));
    cJSON_AddItemToArray(arr, cJSON_CreateNumber(20));

    char *out = cJSON_PrintUnformatted(root);
    printf("created: %s\n", out);

    cJSON *parsed = cJSON_Parse(out);
    printf("name: %s\n", cJSON_GetObjectItem(parsed, "name")->valuestring);
    printf("version: %d\n", cJSON_GetObjectItem(parsed, "version")->valueint);
    printf("items[0]: %d\n",
        cJSON_GetArrayItem(cJSON_GetObjectItem(parsed, "items"), 0)->valueint);

    free(out);
    cJSON_Delete(root);
    cJSON_Delete(parsed);
    return 0;
}
EOF

run_test "jsmn" "$W/jsmn" \
    "gcc -I $W/jsmn -o normal test.c" \
    "test_jsmn.c" \
    '#include "jsmn.h"' '#include "combined.h"' \
    's|#define JSMN_STATIC||' ""

run_test "sds" "$W/sds" \
    "gcc -I $W/sds -o normal test.c $W/sds/sds.c" \
    "test_sds.c" \
    '#include "sds.h"' '#include "combined.h"' \
    "" ""

run_test "tinyexpr" "$W/tinyexpr" \
    "gcc -I $W/tinyexpr -o normal test.c $W/tinyexpr/tinyexpr.c -lm" \
    "test_tinyexpr.c" \
    '#include "tinyexpr.h"' '#include "combined.h"' \
    "" "-lm"

run_test "logc" "$W/logc" \
    "gcc -I $W/logc/src -o normal test.c $W/logc/src/log.c" \
    "test_logc.c" \
    '#include "log.h"' '#include "combined.h"' \
    "" ""

run_test "cjson" "$W/cJSON" \
    "gcc -I $W/cJSON -o normal test.c $W/cJSON/cJSON.c -lm" \
    "test_cjson.c" \
    '#include "cJSON.h"' '#include "combined.h"' \
    "" "-lm"

header "RESULTS"
printf "$RESULTS\n\n"
TOTAL=$((PASS+FAIL))
printf "  \033[1m%d passed, %d failed out of %d\033[0m\n\n" "$PASS" "$FAIL" "$TOTAL"
[ "$FAIL" -eq 0 ]
