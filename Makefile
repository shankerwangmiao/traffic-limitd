TARGET := traffic-limitd
LIBS := pkg/bpftrafficlimiter/libebpf-traffic-limiter
GO ?= go

CLEAN_LIBS := $(addprefix clean-,$(LIBS))

all: $(TARGET)

$(TARGET): $(LIBS)
	$(GO) build -o $@
$(LIBS):
	$(MAKE) --directory=$@
clean: $(CLEAN_LIBS)
	true
$(CLEAN_LIBS):
	$(MAKE) --directory=$(@:clean-%=%) clean

.PHONY: all $(TARGET) $(LIBS) clean $(CLEAN_LIBS)
