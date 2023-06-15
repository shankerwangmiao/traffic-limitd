DAEMON_SRC := src/log.c src/tcbpf_util.c src/rtnl_util.c
EBPF_SRC := src/cgroup_rate_limit.bpf.c

BPFCC := clang -target bpf -O2 -g -I./bpf-include

OBJ_DIR := objs

DAEMON_C_OBJS := $(DAEMON_SRC:%.c=$(OBJ_DIR)/%.o) $(S_TASK_C_SRC:%.c=$(OBJ_DIR)/%.o)
BPF_OBJS := $(EBPF_SRC:%.bpf.c=$(OBJ_DIR)/%.o)
BPF_GEN_HEADERS := $(addprefix $(OBJ_DIR)/generated/include/,$(notdir $(EBPF_SRC:%.bpf.c=%.skel.h)))
C_OBJS := $(DAEMON_C_OBJS)

TARGET := $(OBJ_DIR)/libebpf-traffic-limiter.a

OBJS := $(C_OBJS) $(ASM_OBJS) $(BPF_OBJS)

INCS := -I./include

COMMON_CFLAGS := -Wall -Werror -g -O2 -Wextra -Wstrict-prototypes $(INCS) -I$(OBJ_DIR)/generated/include

CFLAGS += $(COMMON_CFLAGS)

LDLIBS += $(shell pkg-config --cflags --libs libbpf)

LDFLAGS += -g

DEPDIR := .deps
DEPFLAGS = -MT $@ -MMD -MP -MF $(OBJ_DIR)/$(DEPDIR)/$*.d

COMPILE.c = $(CC) $(DEPFLAGS) $(CFLAGS) $(CPPFLAGS) -c
COMPILE.bpf = $(BPFCC) $(DEPFLAGS) $(COMMON_CFLAGS) $(CPPFLAGS) -c
BPFTOOL ?= bpftool

HDR_GEN_TAG := $(OBJ_DIR)/.header_generated

all: $(TARGET)

$(BPF_OBJS) : $(OBJ_DIR)/%.o : %.bpf.c $(OBJ_DIR)/$(DEPDIR)/%.d
	mkdir -p $(dir $@)
	$(COMPILE.bpf) -o $@ $<
$(BPF_GEN_HEADERS) : $(OBJ_DIR)/generated/include/%.skel.h : $(OBJ_DIR)/src/%.o
	mkdir -p $(dir $@)
	$(BPFTOOL) gen skeleton $< > $@
$(C_OBJS) : $(OBJ_DIR)/%.o : %.c $(OBJ_DIR)/$(DEPDIR)/%.d $(HDR_GEN_TAG)
	mkdir -p $(dir $@)
	$(COMPILE.c) -o $@ $<

DEPFILES := $(OBJS:$(OBJ_DIR)/%.o=$(OBJ_DIR)/$(DEPDIR)/%.d)

$(DEPFILES):
	mkdir -p $(dir $@)
include $(wildcard $(DEPFILES))

$(OBJ_DIR)/libebpf-traffic-limiter.a : $(DAEMON_C_OBJS) $(DAEMON_ASM_OBJS)
	$(AR) -cr $@ $^

clean:
	rm -rf $(OBJ_DIR)

$(HDR_GEN_TAG): $(BPF_GEN_HEADERS)
	touch $@

.PHONY: all clean
