# Host build of the on-device voice code.
#   make -C tests/voice check   resolver tests (no dependencies)
#   make -C tests/voice cli     the whole chain against a real calcs phone
SRC = ../../kanaha-audio-app/app/src/main/cpp/voice
AXIS2_SRC ?= $(HOME)/repos/axis-axis2-c-core
AXIS2_LIB ?= $(AXIS2_SRC)/deploy/lib
CFLAGS ?= -std=gnu99 -g -O1 -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined

test_resolver: test_resolver.c $(SRC)/kanaha_resolver.c $(SRC)/kanaha_resolver.h
	$(CC) $(CFLAGS) -o $@ test_resolver.c $(SRC)/kanaha_resolver.c -lm

test_answer: test_answer.c $(SRC)/kanaha_answer.c $(SRC)/kanaha_resolver.c $(SRC)/*.h
	$(CC) $(CFLAGS) -o $@ test_answer.c $(SRC)/kanaha_answer.c $(SRC)/kanaha_resolver.c -lm

check: test_resolver test_answer
	./test_resolver
	./test_answer

# The h2 client is compiled from the Axis2/C checkout, not copied.
kanaha_voice_cli: kanaha_voice_cli.c $(SRC)/*.c $(SRC)/*.h
	$(CC) $(CFLAGS) -I$(AXIS2_SRC)/include -I$(AXIS2_SRC)/util/include -I/usr/include/json-c \
	    -o $@ kanaha_voice_cli.c $(SRC)/kanaha_resolver.c $(SRC)/kanaha_answer.c \
	    $(SRC)/kanaha_calc.c $(AXIS2_SRC)/src/core/transport/h2/sender/axis2_h2_json_client.c \
	    -L$(AXIS2_LIB) -Wl,-rpath,$(AXIS2_LIB) -laxutil -ljson-c -lnghttp2 -lssl -lcrypto -lm

cli: kanaha_voice_cli

clean:
	rm -f test_resolver test_answer kanaha_voice_cli

.PHONY: check cli clean
