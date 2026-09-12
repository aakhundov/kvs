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
BUILD := $(HERE)build

SRCS := $(wildcard $(SRC)/*.c)
HDRS := $(wildcard $(SRC)/*.h)
OBJS := $(SRCS:$(SRC)/%.c=$(BUILD)/%.o)

APP := kvs
SERVER_OBJ = $(BUILD)/$(APP).o
SERVER_EXEC := $(BUILD)/$(APP)

LIB_OBJS = $(filter-out $(SERVER_OBJ),$(OBJS))

DEPS := $(OBJS:%=%.d)
BUILD_FLAGS := $(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) $(LDLIBS)
FLAGS_STAMP := $(BUILD)/.flags

FORMAT_FILES := $(SRCS) $(HDRS)

.PHONY: run doctor clean format format-check tidy check force

$(SERVER_EXEC): $(LIB_OBJS) $(SERVER_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/%.o: $(SRC)/%.c $(FLAGS_STAMP) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -MMD -MP -MF $@.d -c $< -o $@

$(FLAGS_STAMP): force | $(BUILD)
	@echo '$(BUILD_FLAGS)' | cmp -s - $@ || echo '$(BUILD_FLAGS)' > $@

$(BUILD):
	mkdir -p $@

run: $(SERVER_EXEC)
	@$(RUN_ENV) $< $(ARGS)

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
	$(CLANG_TIDY) $(SRCS) -- $(CFLAGS) $(CPPFLAGS)

check: format-check tidy

-include $(DEPS)
