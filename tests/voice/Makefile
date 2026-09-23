# Host build of the on-device resolver's tests: make -C tests/voice check
SRC = ../../kanaha-audio-app/app/src/main/cpp/voice
CFLAGS ?= -std=gnu99 -g -O1 -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined

test_resolver: test_resolver.c $(SRC)/kanaha_resolver.c $(SRC)/kanaha_resolver.h
	$(CC) $(CFLAGS) -o $@ test_resolver.c $(SRC)/kanaha_resolver.c -lm

check: test_resolver
	./test_resolver

clean:
	rm -f test_resolver

.PHONY: check clean
