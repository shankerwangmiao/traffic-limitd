S_TASK_C_SRC = \
	s_task/s_list.c \
	s_task/s_event.c \
	s_task/s_task.c
S_TASK_ASM_SRC = \
	s_task/jump_gas.S \
	s_task/make_gas.S

SRC = src/main.c src/se_libs.c src/log.c

OBJ_DIR = objs

C_OBJS := $(SRC:%.c=$(OBJ_DIR)/%.o) $(S_TASK_C_SRC:%.c=$(OBJ_DIR)/%.o)
ASM_OBJS := $(S_TASK_ASM_SRC:%.S=$(OBJ_DIR)/%.o)
TARGET := $(OBJ_DIR)/main

OBJS := $(C_OBJS) $(ASM_OBJS)

INCS := -I./include

CFLAGS += -Wall -Werror -g -Wextra -Wstrict-prototypes -Wno-unused-parameter -Wno-deprecated-declarations $(INCS)

LDLIBS += $(shell pkg-config --cflags --libs libsystemd)

LDFLAGS += -g

DEPDIR := .deps
DEPFLAGS = -MT $@ -MMD -MP -MF $(OBJ_DIR)/$(DEPDIR)/$*.d

COMPILE.c = $(CC) $(DEPFLAGS) $(CFLAGS) $(CPPFLAGS) -c

all: $(TARGET)

$(C_OBJS) : $(OBJ_DIR)/%.o : %.c $(OBJ_DIR)/$(DEPDIR)/%.d
	mkdir -p $(dir $@)
	$(COMPILE.c) -o $@ $<
$(ASM_OBJS) : $(OBJ_DIR)/%.o : %.S $(OBJ_DIR)/$(DEPDIR)/%.d
	mkdir -p $(dir $@)
	$(COMPILE.c) -o $@ $<

DEPFILES := $(OBJS:$(OBJ_DIR)/%.o=$(OBJ_DIR)/$(DEPDIR)/%.d)

$(DEPFILES):
	mkdir -p $(dir $@)
include $(wildcard $(DEPFILES))

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

clean:
	rm -rf $(OBJ_DIR)

.PHONY: all clean
