BREW_LLVM := /opt/homebrew/opt/llvm/bin
LLVM_BIN  := $(if $(wildcard $(BREW_LLVM)/clang),$(BREW_LLVM)/,)
ifeq ($(origin CC),default)
  CC := $(LLVM_BIN)clang
endif
CLANG_FORMAT := $(LLVM_BIN)clang-format
CLANG_TIDY   := $(LLVM_BIN)clang-tidy

HERE := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

CFLAGS := $(shell cat $(HERE)compile_flags.txt) \
          -g3 -O0 -fno-omit-frame-pointer -Werror
CPPFLAGS :=
LDFLAGS :=
LDLIBS :=

LEAK_OPT := $(if $(filter $(LLVM_BIN)clang,$(CC)),detect_leaks=1:,)
RUN_ENV  := ASAN_OPTIONS=$(LEAK_OPT)detect_stack_use_after_return=1 \
            LSAN_OPTIONS=use_globals=0:use_stacks=0:use_registers=0:use_tls=0

SRC := $(HERE)src
TESTS := $(HERE)tests
VENDOR := $(HERE)vendor
BUILD := $(HERE)build

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
TEST_CPPFLAGS := -I$(SRC) -I$(VENDOR)/utest -D_DARWIN_C_SOURCE

LIB_OBJS = $(filter-out $(SERVER_OBJ),$(OBJS))

DEPS := $(OBJS:%=%.d) $(TEST_OBJS:%=%.d)
BUILD_FLAGS := $(CC) $(CFLAGS) $(CPPFLAGS) $(TEST_CPPFLAGS) $(LDFLAGS) $(LDLIBS)
FLAGS_STAMP := $(BUILD)/.flags

FORMAT_FILES := $(SRCS) $(HDRS) $(TEST_SRCS) $(TEST_HDRS)
TIDY_FILES := $(SRCS) $(TEST_SRCS)

.PHONY: run test doctor clean format format-check tidy check force

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

test: $(TEST_EXEC) $(SERVER_EXEC)
	@$(RUN_ENV) KVS_SERVER=$(SERVER_EXEC) $< $(ARGS)

doctor:
	@echo "CC     = $(CC)"
	@$(CC) --version | head -1
	@echo "target = `$(CC) -print-target-triple`"
	@echo "tidy   = $(CLANG_TIDY)"
	@echo "leaks  = $(if $(LEAK_OPT),on,off)"

clean:
	rm -rf $(BUILD)

format:
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

# the gate's counterpart to format: reports and fails, rewrites nothing
format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_FILES)

tidy:
	$(CLANG_TIDY) $(TIDY_FILES) -- $(CFLAGS) $(CPPFLAGS) $(TEST_CPPFLAGS)

check: format-check tidy test

-include $(DEPS)
