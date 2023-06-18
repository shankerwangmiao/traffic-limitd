CLIENT_SRC := client.c
OBJ_DIR := objs

CLIENT_C_OBJS := $(CLIENT_SRC:%.c=$(OBJ_DIR)/%.o)
C_OBJS := $(CLIENT_C_OBJS)

TARGET := $(OBJ_DIR)/traffic-limit-hook

OBJS := $(C_OBJS)

COMMON_CFLAGS := -Wall -Werror -g -O2 -Wextra -Wstrict-prototypes -Wno-error=stringop-overflow $(INCS)

CFLAGS += $(COMMON_CFLAGS)

LDLIBS +=
LDFLAGS += -g

DEPDIR := .deps
DEPFLAGS = -MT $@ -MMD -MP -MF $(OBJ_DIR)/$(DEPDIR)/$*.d

COMPILE.c = $(CC) $(DEPFLAGS) $(CFLAGS) $(CPPFLAGS) -c

all: $(TARGET)

$(C_OBJS) : $(OBJ_DIR)/%.o : %.c $(OBJ_DIR)/$(DEPDIR)/%.d $(HDR_GEN_TAG)
	mkdir -p $(dir $@)
	$(COMPILE.c) -o $@ $<

DEPFILES := $(OBJS:$(OBJ_DIR)/%.o=$(OBJ_DIR)/$(DEPDIR)/%.d)

$(DEPFILES):
	mkdir -p $(dir $@)
include $(wildcard $(DEPFILES))

$(OBJ_DIR)/traffic-limit-hook : $(CLIENT_C_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

clean:
	rm -rf $(OBJ_DIR)

.PHONY: all clean
