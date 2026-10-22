CC ?= cc
CXX ?= c++
CPPFLAGS += -D_GNU_SOURCE -Iinclude
CFLAGS ?= -std=c11 -O2 -g
CFLAGS += -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes
LDLIBS += -lm -pthread -lrt
CORE = src/tasks.c src/config.c src/scheduler.c src/protocol.c src/ipc.c
SOURCES = $(CORE) src/serial.c src/driver.c src/report.c src/main.c
.PHONY: all test clean sanitize
all: build/host
build:
	mkdir -p build
build/host: $(SOURCES) include/host.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@
build/test_core: tests/test_core.c $(CORE) include/host.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_core.c $(CORE) $(LDFLAGS) $(LDLIBS) -o $@
build/protocol.o: src/protocol.c include/host.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
build/interop_esp: tests/test_interop.cpp build/protocol.o ../nodo1ESP/ESP_RT_Timer/src/protocol.cpp ../nodo1ESP/ESP_RT_Timer/src/protocol.h
	$(CXX) $(CPPFLAGS) -std=c++11 -Wall -Wextra $< ../nodo1ESP/ESP_RT_Timer/src/protocol.cpp build/protocol.o $(LDFLAGS) -o $@
build/interop_pico: tests/test_interop.cpp build/protocol.o ../node2/src/protocol.cpp ../node2/src/crc16.cpp ../node2/include/protocol.h ../node2/include/crc16.h
	$(CXX) $(CPPFLAGS) -I../node2/include -DTEST_PICO -std=c++11 -Wall -Wextra $< ../node2/src/protocol.cpp ../node2/src/crc16.cpp build/protocol.o $(LDFLAGS) -o $@
test: build/host build/test_core build/interop_esp build/interop_pico
	./build/test_core
	./build/interop_esp
	./build/interop_pico
	python3 tests/integration.py
sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS='-std=c11 -O1 -g -Wall -Wextra -Wpedantic -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined' test
clean:
	rm -rf build
