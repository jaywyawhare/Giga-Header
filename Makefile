CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -O2 -Wno-format-truncation -Wno-stringop-truncation
LIBS = -ljson-c
TARGET = server
SOURCE = server.c
T = .giga-test

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCE) $(LIBS)

clean:
	rm -f $(TARGET)

clean-test:
	rm -rf $(T)

install-deps:
	@if command -v pacman > /dev/null 2>&1; then \
		sudo pacman -S --needed json-c git; \
	elif command -v apt-get > /dev/null 2>&1; then \
		sudo apt-get update && sudo apt-get install -y libjson-c-dev git; \
	elif command -v dnf > /dev/null 2>&1; then \
		sudo dnf install -y json-c-devel git; \
	elif command -v brew > /dev/null 2>&1; then \
		brew install json-c git; \
	else \
		echo "Unsupported package manager. Install json-c and git manually."; \
		exit 1; \
	fi

run: $(TARGET)
	./$(TARGET) serve

PASS = printf "  \033[32mPASS\033[0m  %s\n"
FAIL = printf "  \033[31mFAIL\033[0m  %s\n"

define git_init
	cd $(1) && rm -rf .git && git init -q && git add -A && git commit -q -m init
endef

test: test-smoke test-integration

# --- smoke: generate fixtures inline, convert, check output contains expected patterns ---

test-smoke: $(TARGET)
	@mkdir -p $(T)/test1-simple
	@printf '%s\n' 'int factorial(int n) {' '    return n <= 1 ? 1 : n * factorial(n - 1);' '}' > $(T)/test1-simple/math.c
	@$(call git_init,$(T)/test1-simple)
	@./$(TARGET) $(T)/test1-simple -o $(T)/out1.h >/dev/null 2>&1 || true
	@grep -q factorial $(T)/out1.h && $(PASS) "single .c" || { $(FAIL) "single .c"; exit 1; }
	@mkdir -p $(T)/test2-local-headers
	@printf '%s\n' 'typedef struct { float x, y; } vec2;' 'vec2 vec2_add(vec2 a, vec2 b);' > $(T)/test2-local-headers/vec2.h
	@printf '%s\n' 'vec2 vec2_add(vec2 a, vec2 b) {' '    return (vec2){ a.x + b.x, a.y + b.y };' '}' > $(T)/test2-local-headers/vec2.c
	@$(call git_init,$(T)/test2-local-headers)
	@./$(TARGET) $(T)/test2-local-headers -o $(T)/out2.h >/dev/null 2>&1 || true
	@grep -q vec2_add $(T)/out2.h && $(PASS) "header inlining" || { $(FAIL) "header inlining"; exit 1; }
	@mkdir -p $(T)/test3-nested
	@echo '#define APP_NAME "MyApp"' > $(T)/test3-nested/config.h
	@echo 'typedef struct { int id; } Record;' > $(T)/test3-nested/data.h
	@printf '%s\n' '#include "config.h"' '#include "data.h"' > $(T)/test3-nested/app.h
	@printf '%s\n' '#include "app.h"' 'void app_init(void) { (void)0; }' > $(T)/test3-nested/app.c
	@$(call git_init,$(T)/test3-nested)
	@./$(TARGET) $(T)/test3-nested -o $(T)/out3.h >/dev/null 2>&1 || true
	@grep -q APP_NAME $(T)/out3.h && $(PASS) "nested headers" || { $(FAIL) "nested headers"; exit 1; }
	@mkdir -p $(T)/test4-external
	@printf '%s\n' '#include <json-c/json.h>' 'void lib_init(void) { (void)0; }' > $(T)/test4-external/lib.c
	@$(call git_init,$(T)/test4-external)
	@./$(TARGET) $(T)/test4-external -o $(T)/out4.h >/dev/null 2>&1 || true
	@grep -q 'json-c/json.h' $(T)/out4.h && $(PASS) "external deps" || { $(FAIL) "external deps"; exit 1; }
	@mkdir -p $(T)/test5-guards
	@printf '%s\n' '#ifndef GUARDED_H' '#define GUARDED_H' '#ifdef USE_DOUBLE' 'typedef double number_t;' '#else' 'typedef float number_t;' '#endif' 'number_t guarded_add(number_t a, number_t b);' '#endif' > $(T)/test5-guards/guarded.h
	@printf '%s\n' '#include "guarded.h"' 'number_t guarded_add(number_t a, number_t b) {' '    return a + b;' '}' > $(T)/test5-guards/guarded.c
	@$(call git_init,$(T)/test5-guards)
	@./$(TARGET) $(T)/test5-guards -o $(T)/out5.h >/dev/null 2>&1 || true
	@grep -q GUARDED_H $(T)/out5.h && $(PASS) "guard includes" || { $(FAIL) "guard includes"; exit 1; }

# --- integration: GitHub repos, needs network ---

test-integration: $(TARGET)
	@mkdir -p $(T)
	@./$(TARGET) https://github.com/zserge/jsmn.git -o $(T)/gh-jsmn.h > $(T)/jsmn.log 2>&1; grep -q "strategy: compile feedback" $(T)/jsmn.log || { cat $(T)/jsmn.log; $(FAIL) "jsmn — expected strategy"; exit 1; }
	@grep -q jsmn $(T)/gh-jsmn.h && gcc -fsyntax-only -x c $(T)/gh-jsmn.h 2>/dev/null \
		&& $(PASS) "jsmn (compiles)" || { $(FAIL) "jsmn"; exit 1; }
	@./$(TARGET) https://github.com/DaveGamble/cJSON.git -o $(T)/gh-cjson.h 2>&1
	@grep -q cJSON $(T)/gh-cjson.h && gcc -fsyntax-only -x c $(T)/gh-cjson.h 2>/dev/null \
		&& $(PASS) "cJSON (compiles)" || { $(FAIL) "cJSON"; exit 1; }
	@./$(TARGET) https://github.com/antirez/sds.git -o $(T)/gh-sds.h 2>&1
	@grep -q sds $(T)/gh-sds.h \
		&& $(PASS) "sds" || { $(FAIL) "sds"; exit 1; }
	@./$(TARGET) https://github.com/codeplea/tinyexpr.git -o $(T)/gh-tinyexpr.h >/dev/null 2>&1 || true
	@grep -q tinyexpr $(T)/gh-tinyexpr.h && gcc -fsyntax-only -x c $(T)/gh-tinyexpr.h 2>/dev/null \
		&& $(PASS) "tinyexpr (compiles)" || { $(FAIL) "tinyexpr"; exit 1; }
	@./$(TARGET) https://github.com/rxi/log.c.git -o $(T)/gh-logc.h 2>&1
	@grep -q log $(T)/gh-logc.h \
		&& $(PASS) "log.c" || { $(FAIL) "log.c"; exit 1; }

# --- verify-local: build fixture normally, convert to header-only, diff output ---

test-verify-local: $(TARGET)
	@mkdir -p $(T)
	@# simple (factorial)
	@rm -rf $(T)/vl-simple && mkdir -p $(T)/vl-simple
	@printf '%s\n' 'int factorial(int n) {' '    return n <= 1 ? 1 : n * factorial(n - 1);' '}' > $(T)/vl-simple/math.c
	@$(call git_init,$(T)/vl-simple)
	@printf '#include <stdio.h>\nextern int factorial(int n);\nint main(void) {\n    for (int i = 0; i <= 10; i++)\n        printf("factorial(%%d) = %%d\\n", i, factorial(i));\n    return 0;\n}\n' > $(T)/vl-simple/test.c
	@cd $(T)/vl-simple && gcc -o normal test.c math.c 2>/dev/null || { $(FAIL) "simple — normal compile failed"; exit 1; }
	@cd $(T)/vl-simple && ./normal > out_normal.txt
	@./$(TARGET) $(T)/vl-simple -o $(T)/vl-simple/combined.h >/dev/null 2>&1 || { $(FAIL) "simple — generation failed"; exit 1; }
	@sed 's|extern int factorial|#include "combined.h"|' $(T)/vl-simple/test.c > $(T)/vl-simple/test_ho.c
	@cd $(T)/vl-simple && gcc -o ho test_ho.c 2>/dev/null || { $(FAIL) "simple — header-only compile failed"; exit 1; }
	@cd $(T)/vl-simple && ./ho > out_ho.txt
	@diff -q $(T)/vl-simple/out_normal.txt $(T)/vl-simple/out_ho.txt >/dev/null 2>&1 \
		&& $(PASS) "simple" || { $(FAIL) "simple — output differs"; exit 1; }
	@# vec2
	@rm -rf $(T)/vl-vec2 && mkdir -p $(T)/vl-vec2
	@printf '#ifndef VEC2_H\n#define VEC2_H\ntypedef struct { float x, y; } vec2;\nvec2 vec2_add(vec2 a, vec2 b);\n#endif\n' > $(T)/vl-vec2/vec2.h
	@printf '#include "vec2.h"\nvec2 vec2_add(vec2 a, vec2 b) {\n    return (vec2){ a.x + b.x, a.y + b.y };\n}\n' > $(T)/vl-vec2/vec2.c
	@$(call git_init,$(T)/vl-vec2)
	@printf '#include <stdio.h>\n#include "vec2.h"\nint main(void) {\n    vec2 a = {1.0f, 2.0f};\n    vec2 b = {3.0f, 4.0f};\n    vec2 c = vec2_add(a, b);\n    printf("{%%.1f,%%.1f}\\n", c.x, c.y);\n    return 0;\n}\n' > $(T)/vl-vec2/test.c
	@cd $(T)/vl-vec2 && gcc -o normal test.c vec2.c 2>/dev/null || { $(FAIL) "vec2 — normal compile failed"; exit 1; }
	@cd $(T)/vl-vec2 && ./normal > out_normal.txt
	@./$(TARGET) $(T)/vl-vec2 -o $(T)/vl-vec2/combined.h >/dev/null 2>&1 || { $(FAIL) "vec2 — generation failed"; exit 1; }
	@sed 's|#include "vec2.h"|#include "combined.h"|' $(T)/vl-vec2/test.c > $(T)/vl-vec2/test_ho.c
	@cd $(T)/vl-vec2 && gcc -o ho test_ho.c 2>/dev/null || { $(FAIL) "vec2 — header-only compile failed"; exit 1; }
	@cd $(T)/vl-vec2 && ./ho > out_ho.txt
	@diff -q $(T)/vl-vec2/out_normal.txt $(T)/vl-vec2/out_ho.txt >/dev/null 2>&1 \
		&& $(PASS) "vec2" || { $(FAIL) "vec2 — output differs"; exit 1; }
	@# nested
	@rm -rf $(T)/vl-nested && mkdir -p $(T)/vl-nested
	@printf '#define APP_NAME "MyApp"\n' > $(T)/vl-nested/config.h
	@printf 'typedef struct { int id; } Record;\n' > $(T)/vl-nested/data.h
	@printf '#include "config.h"\n#include "data.h"\n' > $(T)/vl-nested/app.h
	@printf '#include "app.h"\nvoid app_init(void) { (void)0; }\n' > $(T)/vl-nested/app.c
	@$(call git_init,$(T)/vl-nested)
	@printf '#include <stdio.h>\n#include "app.h"\nextern void app_init(void);\nint main(void) {\n    printf("app: %%s\\n", APP_NAME);\n    Record r = {42};\n    printf("record: %%d\\n", r.id);\n    app_init();\n    printf("init: ok\\n");\n    return 0;\n}\n' > $(T)/vl-nested/test.c
	@cd $(T)/vl-nested && gcc -o normal test.c app.c 2>/dev/null || { $(FAIL) "nested — normal compile failed"; exit 1; }
	@cd $(T)/vl-nested && ./normal > out_normal.txt
	@./$(TARGET) $(T)/vl-nested -o $(T)/vl-nested/combined.h >/dev/null 2>&1 || { $(FAIL) "nested — generation failed"; exit 1; }
	@sed 's|#include "app.h"|#include "combined.h"|' $(T)/vl-nested/test.c > $(T)/vl-nested/test_ho.c
	@cd $(T)/vl-nested && gcc -o ho test_ho.c 2>/dev/null || { $(FAIL) "nested — header-only compile failed"; exit 1; }
	@cd $(T)/vl-nested && ./ho > out_ho.txt
	@diff -q $(T)/vl-nested/out_normal.txt $(T)/vl-nested/out_ho.txt >/dev/null 2>&1 \
		&& $(PASS) "nested" || { $(FAIL) "nested — output differs"; exit 1; }
	@# guards
	@rm -rf $(T)/vl-guards && mkdir -p $(T)/vl-guards
	@printf '#ifndef GUARDED_H\n#define GUARDED_H\n#ifdef USE_DOUBLE\ntypedef double number_t;\n#else\ntypedef float number_t;\n#endif\nnumber_t guarded_add(number_t a, number_t b);\n#endif\n' > $(T)/vl-guards/guarded.h
	@printf '#include "guarded.h"\nnumber_t guarded_add(number_t a, number_t b) {\n    return a + b;\n}\n' > $(T)/vl-guards/guarded.c
	@$(call git_init,$(T)/vl-guards)
	@printf '#include <stdio.h>\n#define USE_DOUBLE\n#include "guarded.h"\nint main(void) {\n    number_t a = 1.5, b = 2.3;\n    number_t c = guarded_add(a, b);\n    printf("sizeof=%%zu add=%%.1f\\n", sizeof(number_t), (double)c);\n    return 0;\n}\n' > $(T)/vl-guards/test.c
	@cd $(T)/vl-guards && gcc -DUSE_DOUBLE -o normal test.c guarded.c 2>/dev/null || { $(FAIL) "guards — normal compile failed"; exit 1; }
	@cd $(T)/vl-guards && ./normal > out_normal.txt
	@./$(TARGET) $(T)/vl-guards -o $(T)/vl-guards/combined.h >/dev/null 2>&1 || { $(FAIL) "guards — generation failed"; exit 1; }
	@sed 's|#include "guarded.h"|#include "combined.h"|' $(T)/vl-guards/test.c > $(T)/vl-guards/test_ho.c
	@cd $(T)/vl-guards && gcc -o ho test_ho.c 2>/dev/null || { $(FAIL) "guards — header-only compile failed"; exit 1; }
	@cd $(T)/vl-guards && ./ho > out_ho.txt
	@diff -q $(T)/vl-guards/out_normal.txt $(T)/vl-guards/out_ho.txt >/dev/null 2>&1 \
		&& $(PASS) "guards" || { $(FAIL) "guards — output differs"; exit 1; }

# --- verify: behavioral equivalence on GitHub repos, needs network ---

define clone_if_needed
	@if [ ! -d "$(T)/vr-$(1)-repo/.git" ]; then \
		git clone --depth 1 $(2) $(T)/vr-$(1)-repo 2>/dev/null || { $(FAIL) "$(1) — clone failed"; exit 1; }; \
	fi
endef

test-verify: $(TARGET)
	@mkdir -p $(T)
	@# jsmn
	$(call clone_if_needed,jsmn,https://github.com/zserge/jsmn.git)
	@rm -rf $(T)/vr-jsmn && mkdir -p $(T)/vr-jsmn
	@printf '#include <stdio.h>\n#include <string.h>\n#define JSMN_STATIC\n#include "jsmn.h"\nint main(void) {\n    const char *json = "{\\\"name\\\":\\\"test\\\",\\\"value\\\":42}";\n    jsmn_parser parser;\n    jsmntok_t tokens[16];\n    jsmn_init(&parser);\n    int r = jsmn_parse(&parser, json, strlen(json), tokens, 16);\n    printf("tokens: %%d\\n", r);\n    printf("type[0]: %%d\\n", tokens[0].type);\n    printf("type[1]: %%d\\n", tokens[1].type);\n    return 0;\n}\n' > $(T)/vr-jsmn/test.c
	@cd $(T)/vr-jsmn && gcc -I $(CURDIR)/$(T)/vr-jsmn-repo -o normal test.c 2>/dev/null || { $(FAIL) "jsmn — normal compile failed"; exit 1; }
	@cd $(T)/vr-jsmn && ./normal > out_normal.txt
	@./$(TARGET) $(T)/vr-jsmn-repo -o $(T)/vr-jsmn/combined.h >/dev/null 2>&1 || true
	@sed 's|#define JSMN_STATIC||;s|#include "jsmn.h"|#include "combined.h"|' $(T)/vr-jsmn/test.c > $(T)/vr-jsmn/test_ho.c
	@cd $(T)/vr-jsmn && gcc -o ho test_ho.c 2>/dev/null || { $(FAIL) "jsmn — header-only compile failed"; exit 1; }
	@cd $(T)/vr-jsmn && ./ho > out_ho.txt
	@diff -q $(T)/vr-jsmn/out_normal.txt $(T)/vr-jsmn/out_ho.txt >/dev/null 2>&1 \
		&& $(PASS) "jsmn" || { $(FAIL) "jsmn — output differs"; exit 1; }
	@# sds
	$(call clone_if_needed,sds,https://github.com/antirez/sds.git)
	@rm -rf $(T)/vr-sds && mkdir -p $(T)/vr-sds
	@printf '#include <stdio.h>\n#include "sds.h"\nint main(void) {\n    sds s = sdsnew("Hello");\n    s = sdscat(s, " World");\n    printf("str: %%s\\n", s);\n    printf("len: %%zu\\n", sdslen(s));\n    sds s2 = sdscatprintf(sdsempty(), "%%d+%%d=%%d", 1, 2, 3);\n    printf("fmt: %%s\\n", s2);\n    sdsfree(s);\n    sdsfree(s2);\n    return 0;\n}\n' > $(T)/vr-sds/test.c
	@cd $(T)/vr-sds && gcc -I $(CURDIR)/$(T)/vr-sds-repo -o normal test.c $(CURDIR)/$(T)/vr-sds-repo/sds.c 2>/dev/null || { $(FAIL) "sds — normal compile failed"; exit 1; }
	@cd $(T)/vr-sds && ./normal > out_normal.txt
	@./$(TARGET) $(T)/vr-sds-repo -o $(T)/vr-sds/combined.h >/dev/null 2>&1 || true
	@sed 's|#include "sds.h"|#include "combined.h"|' $(T)/vr-sds/test.c > $(T)/vr-sds/test_ho.c
	@cd $(T)/vr-sds && gcc -o ho test_ho.c 2>/dev/null || { $(FAIL) "sds — header-only compile failed"; exit 1; }
	@cd $(T)/vr-sds && ./ho > out_ho.txt
	@diff -q $(T)/vr-sds/out_normal.txt $(T)/vr-sds/out_ho.txt >/dev/null 2>&1 \
		&& $(PASS) "sds" || { $(FAIL) "sds — output differs"; exit 1; }
	@# tinyexpr
	$(call clone_if_needed,tinyexpr,https://github.com/codeplea/tinyexpr.git)
	@rm -rf $(T)/vr-tinyexpr && mkdir -p $(T)/vr-tinyexpr
	@printf '#include <stdio.h>\n#include "tinyexpr.h"\nint main(void) {\n    double r1 = te_interp("2+3*4", 0);\n    printf("2+3*4 = %%.1f\\n", r1);\n    double r2 = te_interp("sqrt(9)", 0);\n    printf("sqrt(9) = %%.1f\\n", r2);\n    double r3 = te_interp("(5+3)/2", 0);\n    printf("(5+3)/2 = %%.1f\\n", r3);\n    return 0;\n}\n' > $(T)/vr-tinyexpr/test.c
	@cd $(T)/vr-tinyexpr && gcc -I $(CURDIR)/$(T)/vr-tinyexpr-repo -o normal test.c $(CURDIR)/$(T)/vr-tinyexpr-repo/tinyexpr.c -lm 2>/dev/null || { $(FAIL) "tinyexpr — normal compile failed"; exit 1; }
	@cd $(T)/vr-tinyexpr && ./normal > out_normal.txt
	@./$(TARGET) $(T)/vr-tinyexpr-repo -o $(T)/vr-tinyexpr/combined.h >/dev/null 2>&1 || true
	@sed 's|#include "tinyexpr.h"|#include "combined.h"|' $(T)/vr-tinyexpr/test.c > $(T)/vr-tinyexpr/test_ho.c
	@cd $(T)/vr-tinyexpr && gcc -o ho test_ho.c -lm 2>/dev/null || { $(FAIL) "tinyexpr — header-only compile failed"; exit 1; }
	@cd $(T)/vr-tinyexpr && ./ho > out_ho.txt
	@diff -q $(T)/vr-tinyexpr/out_normal.txt $(T)/vr-tinyexpr/out_ho.txt >/dev/null 2>&1 \
		&& $(PASS) "tinyexpr" || { $(FAIL) "tinyexpr — output differs"; exit 1; }
	@# logc
	$(call clone_if_needed,logc,https://github.com/rxi/log.c.git)
	@rm -rf $(T)/vr-logc && mkdir -p $(T)/vr-logc
	@printf '#include <stdio.h>\n#include <string.h>\n#include "log.h"\nstatic char buf[4096];\nstatic int bpos = 0;\nstatic void cb(log_Event *ev) {\n    bpos += snprintf(buf + bpos, sizeof(buf) - (size_t)bpos,\n        "level=%%d msg=%%s\\n", ev->level, (const char *)ev->fmt);\n}\nint main(void) {\n    log_set_quiet(1);\n    log_add_callback(cb, NULL, LOG_TRACE);\n    log_trace("trace_msg");\n    log_info("info_msg");\n    log_warn("warn_msg");\n    log_error("error_msg");\n    printf("%%s", buf);\n    return 0;\n}\n' > $(T)/vr-logc/test.c
	@cd $(T)/vr-logc && gcc -I $(CURDIR)/$(T)/vr-logc-repo/src -o normal test.c $(CURDIR)/$(T)/vr-logc-repo/src/log.c 2>/dev/null || { $(FAIL) "logc — normal compile failed"; exit 1; }
	@cd $(T)/vr-logc && ./normal > out_normal.txt
	@./$(TARGET) $(T)/vr-logc-repo -o $(T)/vr-logc/combined.h >/dev/null 2>&1 || true
	@sed 's|#include "log.h"|#include "combined.h"|' $(T)/vr-logc/test.c > $(T)/vr-logc/test_ho.c
	@cd $(T)/vr-logc && gcc -o ho test_ho.c 2>/dev/null || { $(FAIL) "logc — header-only compile failed"; exit 1; }
	@cd $(T)/vr-logc && ./ho > out_ho.txt
	@diff -q $(T)/vr-logc/out_normal.txt $(T)/vr-logc/out_ho.txt >/dev/null 2>&1 \
		&& $(PASS) "logc" || { $(FAIL) "logc — output differs"; exit 1; }
	@# cjson
	$(call clone_if_needed,cjson,https://github.com/DaveGamble/cJSON.git)
	@rm -rf $(T)/vr-cjson && mkdir -p $(T)/vr-cjson
	@printf '#include <stdio.h>\n#include <stdlib.h>\n#include "cJSON.h"\nint main(void) {\n    cJSON *root = cJSON_CreateObject();\n    cJSON_AddStringToObject(root, "name", "giga");\n    cJSON_AddNumberToObject(root, "version", 1);\n    cJSON_AddTrueToObject(root, "working");\n    cJSON *arr = cJSON_AddArrayToObject(root, "items");\n    cJSON_AddItemToArray(arr, cJSON_CreateNumber(10));\n    cJSON_AddItemToArray(arr, cJSON_CreateNumber(20));\n    char *out = cJSON_PrintUnformatted(root);\n    printf("created: %%s\\n", out);\n    cJSON *parsed = cJSON_Parse(out);\n    printf("name: %%s\\n", cJSON_GetObjectItem(parsed, "name")->valuestring);\n    printf("version: %%d\\n", cJSON_GetObjectItem(parsed, "version")->valueint);\n    printf("items[0]: %%d\\n",\n        cJSON_GetArrayItem(cJSON_GetObjectItem(parsed, "items"), 0)->valueint);\n    free(out);\n    cJSON_Delete(root);\n    cJSON_Delete(parsed);\n    return 0;\n}\n' > $(T)/vr-cjson/test.c
	@cd $(T)/vr-cjson && gcc -I $(CURDIR)/$(T)/vr-cjson-repo -o normal test.c $(CURDIR)/$(T)/vr-cjson-repo/cJSON.c -lm 2>/dev/null || { $(FAIL) "cjson — normal compile failed"; exit 1; }
	@cd $(T)/vr-cjson && ./normal > out_normal.txt
	@./$(TARGET) $(T)/vr-cjson-repo -o $(T)/vr-cjson/combined.h >/dev/null 2>&1 || true
	@sed 's|#include "cJSON.h"|#include "combined.h"|' $(T)/vr-cjson/test.c > $(T)/vr-cjson/test_ho.c
	@cd $(T)/vr-cjson && gcc -o ho test_ho.c -lm 2>/dev/null || { $(FAIL) "cjson — header-only compile failed"; exit 1; }
	@cd $(T)/vr-cjson && ./ho > out_ho.txt
	@diff -q $(T)/vr-cjson/out_normal.txt $(T)/vr-cjson/out_ho.txt >/dev/null 2>&1 \
		&& $(PASS) "cjson" || { $(FAIL) "cjson — output differs"; exit 1; }

check-libs:
	@pkg-config --exists json-c || (echo "json-c not found. Run 'make install-deps'" && exit 1)
	@which git > /dev/null 2>&1 || (echo "git not found. Install git." && exit 1)
	@echo "All required libraries are available!"

.PHONY: all clean clean-test install-deps run test test-smoke test-integration test-verify test-verify-local check-libs
