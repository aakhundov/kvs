# make's built-in default for CC is cc; clang unless the command line says otherwise
ifeq ($(origin CC),default)
  CC := clang
endif
CLANG_FORMAT := clang-format
CLANG_TIDY   := clang-tidy

HERE := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

SRC := $(HERE)src
TESTS := $(HERE)tests
VENDOR := $(HERE)vendor
BUILD := $(HERE)build

ifeq ($(SANITIZER),ASAN)
  SANITIZER_FLAGS := -fsanitize=address,undefined
  SANITIZER_ENV := ASAN_OPTIONS=detect_leaks=1:detect_stack_use_after_return=1
  BUILD := $(BUILD)/asan
else ifeq ($(SANITIZER),TSAN) 
  SANITIZER_FLAGS := -fsanitize=thread,undefined
  SANITIZER_ENV := TSAN_OPTIONS=atexit_sleep_ms=10
  BUILD := $(BUILD)/tsan
else
  ifneq ($(SANITIZER),)
    $(error bad sanitizer: "$(SANITIZER)")
  endif
  # no sanitizer
  SANITIZER_FLAGS := 
  SANITIZER_ENV := 
endif

CFLAGS := $(shell cat $(HERE)compile_flags.txt) \
          -g3 -O0 -fno-omit-frame-pointer -Werror $(SANITIZER_FLAGS)
CPPFLAGS :=
LDFLAGS :=
LDLIBS :=

RUN_ENV := $(SANITIZER_ENV)

SRCS := $(wildcard $(SRC)/*.c)
HDRS := $(wildcard $(SRC)/*.h)
OBJS := $(SRCS:$(SRC)/%.c=$(BUILD)/%.o)

APP := kvs
SERVER_OBJ = $(BUILD)/$(APP).o
SERVER_EXEC := $(BUILD)/$(APP)

TEST_SRCS := $(wildcard $(TESTS)/*.c) $(wildcard $(TESTS)/support/*.c)
TEST_HDRS := $(wildcard $(TESTS)/support/*.h)
TEST_OBJS := $(TEST_SRCS:$(TESTS)/%.c=$(BUILD)/tests/%.o)
TEST_EXEC := $(BUILD)/$(APP)_tests
TEST_CPPFLAGS := -I$(SRC) -I$(VENDOR)/utest

LIB_OBJS = $(filter-out $(SERVER_OBJ),$(OBJS))

DEPS := $(OBJS:%=%.d) $(TEST_OBJS:%=%.d)
BUILD_FLAGS := $(CC) $(CFLAGS) $(CPPFLAGS) $(TEST_CPPFLAGS) $(LDFLAGS) $(LDLIBS)
FLAGS_STAMP := $(BUILD)/.flags

FORMAT_FILES := $(SRCS) $(HDRS) $(TEST_SRCS) $(TEST_HDRS)
TIDY_FILES := $(SRCS) $(TEST_SRCS)

.PHONY: run run-asan run-tsan test test-asan test-tsan doctor clean format format-check tidy check force

$(SERVER_EXEC): $(LIB_OBJS) $(SERVER_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/%.o: $(SRC)/%.c $(FLAGS_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -MMD -MP -MF $@.d -c $< -o $@

$(TEST_EXEC): $(TEST_OBJS) $(LIB_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/tests/%.o: $(TESTS)/%.c $(FLAGS_STAMP) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(TEST_CPPFLAGS) -MMD -MP -MF $@.d -c $< -o $@

$(FLAGS_STAMP): force | $(BUILD)
	@echo '$(BUILD_FLAGS)' | cmp -s - $@ || echo '$(BUILD_FLAGS)' > $@

$(BUILD):
	mkdir -p $@

run: $(SERVER_EXEC)
	@$(RUN_ENV) $< $(ARGS)

run-asan:
	@$(MAKE) run --no-print-directory SANITIZER=ASAN

run-tsan:
	@$(MAKE) run --no-print-directory SANITIZER=TSAN

test: $(TEST_EXEC) $(SERVER_EXEC)
	@$(RUN_ENV) KVS_SERVER=$(SERVER_EXEC) $< $(ARGS)

test-asan:
	@$(MAKE) test --no-print-directory SANITIZER=ASAN

test-tsan:
	@$(MAKE) test --no-print-directory SANITIZER=TSAN

doctor:
	@echo "CC     = $(CC)"
	@$(CC) --version | head -1
	@echo "target = `$(CC) -print-target-triple`"
	@echo "tidy   = $(CLANG_TIDY)"

clean:
	rm -rf $(BUILD)

format:
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

# the gate's counterpart to format: reports and fails, rewrites nothing
format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_FILES)

tidy:
	$(CLANG_TIDY) $(TIDY_FILES) -- $(CFLAGS) $(CPPFLAGS) $(TEST_CPPFLAGS)

check: format-check tidy test-asan test-tsan

-include $(DEPS)
