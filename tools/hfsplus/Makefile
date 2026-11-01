CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c99 -Wall -Wextra -Werror -Wconversion -Wshadow
CPPFLAGS += -I../../firmware/hfsplus
SOURCES = inspect.c ../../firmware/hfsplus/hfsplus_ro.c ../../firmware/hfsplus/hfsplus_partition.c

.PHONY: all test sanitize clean
all: hfsplus-inspect

ADAPTER_HOST = tests/adapter_host
ADAPTER_SOURCES = tests/adapter_host.c ../../firmware/hfsplus/hfsplus_rb.c \
                  ../../firmware/hfsplus/hfsplus_ro.c \
                  ../../firmware/hfsplus/hfsplus_partition.c

$(ADAPTER_HOST): $(ADAPTER_SOURCES) ../../firmware/hfsplus/hfsplus_ro.h \
                 ../../firmware/hfsplus/hfsplus_partition.h \
                 tests/adapter/host/firmware/export/config.h
	$(CC) -std=gnu99 -O1 -g -Wall -Wextra \
	  -Itests/adapter/host/firmware/export -I../../firmware/hfsplus \
	  -I../../firmware/include $(ADAPTER_SOURCES) -o $@

hfsplus-inspect: $(SOURCES) ../../firmware/hfsplus/hfsplus_ro.h ../../firmware/hfsplus/hfsplus_partition.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) $(LDFLAGS) -o $@

test: hfsplus-inspect api-test $(ADAPTER_HOST)
	./api-test
	python3 tests/test_images.py ./hfsplus-inspect
	python3 tests/test_partitions.py ./hfsplus-inspect
	python3 tests/test_adapter.py ./$(ADAPTER_HOST)

api-test: tests/api_test.c ../../firmware/hfsplus/hfsplus_ro.c ../../firmware/hfsplus/hfsplus_ro.h
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/api_test.c ../../firmware/hfsplus/hfsplus_ro.c $(LDFLAGS) -o $@

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
SAN_CFLAGS = -O1 -g -std=c99 -Wall -Wextra -Werror -Wconversion -Wshadow \
             -fsanitize=address,undefined -fno-omit-frame-pointer
SAN_LDFLAGS = -fsanitize=address,undefined
else
SAN_CFLAGS = -O1 -g -std=c99 -Wall -Wextra -Werror -Wconversion -Wshadow \
             -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie
SAN_LDFLAGS = -fsanitize=address,undefined -no-pie
endif

sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS='$(SAN_CFLAGS)' LDFLAGS='$(SAN_LDFLAGS)' test

test-sanitize: sanitize

clean:
	rm -f hfsplus-inspect api-test $(ADAPTER_HOST)
