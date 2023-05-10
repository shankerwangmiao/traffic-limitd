S_TASK_C_SRC := \
	s_task/s_list.c \
	s_task/s_event.c \
	s_task/s_task.c
S_TASK_ASM_SRC := \
	s_task/jump_gas.S \
	s_task/make_gas.S

DAEMON_SRC := src/main.c src/se_libs.c src/log.c src/unix_sock.c src/sd_bus.c src/cgroup_util.c src/tcbpf_util.c src/rtnl_util.c
CLIENT_SRC := src/client.c
EBPF_SRC := src/cgroup_rate_limit.bpf.c

BPFCC := clang -target bpf -O2 -g -I./bpf-include

OBJ_DIR := objs

DAEMON_C_OBJS := $(DAEMON_SRC:%.c=$(OBJ_DIR)/%.o) $(S_TASK_C_SRC:%.c=$(OBJ_DIR)/%.o)
DAEMON_ASM_OBJS := $(S_TASK_ASM_SRC:%.S=$(OBJ_DIR)/%.o)
CLIENT_C_OBJS := $(CLIENT_SRC:%.c=$(OBJ_DIR)/%.o)
BPF_OBJS := $(EBPF_SRC:%.bpf.c=$(OBJ_DIR)/%.o)
BPF_GEN_HEADERS := $(addprefix $(OBJ_DIR)/generated/include/,$(notdir $(EBPF_SRC:%.bpf.c=%.skel.h)))
C_OBJS := $(DAEMON_C_OBJS) $(CLIENT_C_OBJS)
ASM_OBJS := $(DAEMON_ASM_OBJS)

TARGET := $(OBJ_DIR)/main $(OBJ_DIR)/client

OBJS := $(C_OBJS) $(ASM_OBJS) $(BPF_OBJS)

INCS := -I./include

CFLAGS += -Wall -Werror -g -Wextra -Wstrict-prototypes -Wno-unused-parameter -Wno-deprecated-declarations $(INCS) -I$(OBJ_DIR)/generated/include

LDLIBS += $(shell pkg-config --cflags --libs libsystemd)

LDFLAGS += -g

DEPDIR := .deps
DEPFLAGS = -MT $@ -MMD -MP -MF $(OBJ_DIR)/$(DEPDIR)/$*.d

COMPILE.c = $(CC) $(DEPFLAGS) $(CFLAGS) $(CPPFLAGS) -c
COMPILE.bpf = $(BPFCC) $(DEPFLAGS) $(CFLAGS) $(CPPFLAGS) -c

all: $(TARGET)

$(BPF_OBJS) : $(OBJ_DIR)/%.o : %.bpf.c $(OBJ_DIR)/$(DEPDIR)/%.d
	mkdir -p $(dir $@)
	$(COMPILE.bpf) -o $@ $<
$(BPF_GEN_HEADERS) : $(OBJ_DIR)/generated/include/%.skel.h : $(OBJ_DIR)/src/%.o
	mkdir -p $(dir $@)
	bpftool gen skeleton $< > $@
$(C_OBJS) : $(OBJ_DIR)/%.o : %.c $(OBJ_DIR)/$(DEPDIR)/%.d header_generate
	mkdir -p $(dir $@)
	$(COMPILE.c) -o $@ $<
$(ASM_OBJS) : $(OBJ_DIR)/%.o : %.S $(OBJ_DIR)/$(DEPDIR)/%.d header_generate
	mkdir -p $(dir $@)
	$(COMPILE.c) -o $@ $<

DEPFILES := $(OBJS:$(OBJ_DIR)/%.o=$(OBJ_DIR)/$(DEPDIR)/%.d)

$(DEPFILES):
	mkdir -p $(dir $@)
include $(wildcard $(DEPFILES))

$(OBJ_DIR)/main : $(DAEMON_C_OBJS) $(DAEMON_ASM_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OBJ_DIR)/client : $(CLIENT_C_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

clean:
	rm -rf $(OBJ_DIR)

header_generate: $(BPF_GEN_HEADERS)

.PHONY: all clean header_generate
