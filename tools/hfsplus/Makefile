CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c99 -Wall -Wextra -Werror -Wconversion -Wshadow
CPPFLAGS += -I../../firmware/hfsplus
SOURCES = inspect.c ../../firmware/hfsplus/hfsplus_ro.c ../../firmware/hfsplus/hfsplus_partition.c

.PHONY: all test sanitize clean
all: hfsplus-inspect

hfsplus-inspect: $(SOURCES) ../../firmware/hfsplus/hfsplus_ro.h ../../firmware/hfsplus/hfsplus_partition.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) $(LDFLAGS) -o $@

test: hfsplus-inspect api-test
	./api-test
	python3 tests/test_images.py ./hfsplus-inspect
	python3 tests/test_partitions.py ./hfsplus-inspect

api-test: tests/api_test.c ../../firmware/hfsplus/hfsplus_ro.c ../../firmware/hfsplus/hfsplus_ro.h
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/api_test.c ../../firmware/hfsplus/hfsplus_ro.c $(LDFLAGS) -o $@

sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS='-O1 -g -std=c99 -Wall -Wextra -Werror -Wconversion -Wshadow -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie' LDFLAGS='-fsanitize=address,undefined -no-pie' test

clean:
	rm -f hfsplus-inspect api-test
