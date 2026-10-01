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

ifeq ($(STRESS),1)
  ifneq ($(SANITIZER),TSAN)
    $(error STRESS=1 needs SANITIZER=TSAN)
  endif
  BUILD := $(HERE)build/stress
  STRESS_CPPFLAGS := -DKVS_STRESS_TEST=1
endif

CFLAGS := $(shell cat $(HERE)compile_flags.txt) \
          -g3 -O0 -fno-omit-frame-pointer -Werror $(SANITIZER_FLAGS)
CPPFLAGS := $(STRESS_CPPFLAGS)
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

STRESS_SRC := $(TESTS)/stress/stress.c
STRESS_EXEC := $(BUILD)/$(APP)_stress

SOAK_SRC := $(TESTS)/soak/soak.c
SOAK_OBJS := $(BUILD)/tests/soak/soak.o $(BUILD)/tests/support/server.o
SOAK_EXEC := $(BUILD)/$(APP)_soak

LIB_OBJS = $(filter-out $(SERVER_OBJ),$(OBJS))

DEPS := $(OBJS:%=%.d) $(TEST_OBJS:%=%.d) $(SOAK_OBJS:%=%.d)
BUILD_FLAGS := $(CC) $(CFLAGS) $(CPPFLAGS) $(TEST_CPPFLAGS) $(LDFLAGS) $(LDLIBS)
FLAGS_STAMP := $(BUILD)/.flags

FORMAT_FILES := $(SRCS) $(HDRS) $(TEST_SRCS) $(TEST_HDRS) $(STRESS_SRC) $(SOAK_SRC)
TIDY_FILES := $(SRCS) $(TEST_SRCS) $(STRESS_SRC) $(SOAK_SRC)

.PHONY: run run-asan run-tsan test test-asan test-tsan test-all stress stress-build run-stress \
        run-stress-trial stress-run soak run-soak doctor clean format format-check tidy check force

$(SERVER_EXEC): $(LIB_OBJS) $(SERVER_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/%.o: $(SRC)/%.c $(FLAGS_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -MMD -MP -MF $@.d -c $< -o $@

$(TEST_EXEC): $(TEST_OBJS) $(LIB_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/tests/%.o: $(TESTS)/%.c $(FLAGS_STAMP) | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(TEST_CPPFLAGS) -MMD -MP -MF $@.d -c $< -o $@

$(STRESS_EXEC): $(STRESS_SRC) $(FLAGS_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) $< $(LDLIBS) -o $@

$(SOAK_EXEC): $(SOAK_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

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

test-all: test test-asan test-tsan

stress:
	@$(MAKE) stress-build --no-print-directory SANITIZER=TSAN STRESS=1

stress-build: $(SERVER_EXEC) $(TEST_EXEC) $(STRESS_EXEC)

run-stress:
	@$(MAKE) stress-run --no-print-directory SANITIZER=TSAN STRESS=1 STRESS_SEED=

run-stress-trial:
	$(if $(KVS_STRESS_SEED),,$(error run-stress-trial needs KVS_STRESS_SEED))
	@$(MAKE) stress-run --no-print-directory SANITIZER=TSAN STRESS=1 STRESS_SEED=$(KVS_STRESS_SEED)

stress-run: stress-build
	@$(RUN_ENV) KVS_SERVER=$(SERVER_EXEC) $(STRESS_EXEC) $(TEST_EXEC) $(STRESS_SEED)

soak: $(SERVER_EXEC) $(SOAK_EXEC)

run-soak: soak
	@KVS_SERVER=$(SERVER_EXEC) $(SOAK_EXEC)

doctor:
	@echo "CC     = $(CC)"
	@$(CC) --version | head -1
	@echo "target = `$(CC) -print-target-triple`"
	@echo "tidy   = $(CLANG_TIDY)"

clean:
	rm -rf $(BUILD)

format:
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_FILES)

tidy:
	$(CLANG_TIDY) $(TIDY_FILES) -- $(CFLAGS) $(CPPFLAGS) $(TEST_CPPFLAGS)

check: format-check tidy test-all

-include $(DEPS)
