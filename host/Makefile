CC ?= cc
CXX ?= c++
CPPFLAGS += -D_GNU_SOURCE -Iinclude
CFLAGS ?= -std=c11 -O2 -g
CFLAGS += -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes
LDLIBS += -lm -pthread -lrt
CORE = src/tasks.c src/config.c src/scheduler.c src/protocol.c src/ipc.c src/telemetry.c
SOURCES = $(CORE) src/serial.c src/driver.c src/report.c src/main.c
WIRE = ../common/rt_protocol.h
ESP = ../nodo1ESP/ESP_RT_Timer
PICO = ../node2
CONFIG ?= examples/host.ini
PIO ?= $(if $(wildcard $(HOME)/.platformio/penv/bin/pio),$(HOME)/.platformio/penv/bin/pio,pio)
PICO_SRC = $(wildcard $(PICO)/src/*.cpp) $(wildcard $(PICO)/src/scheduler/*.cpp)
ESP_TESTS = $(addprefix build/esp_,sched_core_test task_parser_test event_queue_test tick_report_test)
.PHONY: all run host run-host setup-usb test clean sanitize
all: build/host
run: build/host
	@sh scripts/run.sh "$(CONFIG)" "$(PIO)"
host run-host: build/host
	./build/host "$(CONFIG)"
setup-usb:
	sudo install -m 0644 scripts/udev/60-rt-pico.rules /etc/udev/rules.d/60-rt-pico.rules
	sudo udevadm control --reload-rules
	sudo udevadm trigger --action=add --subsystem-match=usb --attr-match=idVendor=2e8a --attr-match=idProduct=0003
	@printf 'Permisos BOOTSEL configurados para la sesión local. Vuelve a ejecutar make run.\n'
build:
	mkdir -p build
build/host: $(SOURCES) include/host.h $(WIRE) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@
build/test_core: tests/test_core.c $(CORE) include/host.h $(WIRE) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_core.c $(CORE) $(LDFLAGS) $(LDLIBS) -o $@
build/test_report: tests/test_report.c src/report.c $(CORE) include/host.h $(WIRE) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_report.c src/report.c $(CORE) $(LDFLAGS) $(LDLIBS) -o $@
build/protocol.o: src/protocol.c include/host.h $(WIRE) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
build/ipc.o: src/ipc.c include/host.h $(WIRE) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
build/telemetry.o: src/telemetry.c include/host.h $(WIRE) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
INTEROP_CORE = build/protocol.o build/ipc.o build/telemetry.o
ESP_WIRE = $(addprefix $(ESP)/src/,protocol.cpp event_queue.cpp tick_report.cpp)
PICO_WIRE = $(addprefix $(PICO)/src/,protocol.cpp crc16.cpp event_queue.cpp messages.cpp)
build/interop_esp: tests/test_interop.cpp $(INTEROP_CORE) $(ESP_WIRE) $(wildcard $(ESP)/src/lib/*.h) $(WIRE)
	$(CXX) $(CPPFLAGS) -I$(ESP)/src -std=c++17 -Wall -Wextra $< $(ESP_WIRE) $(INTEROP_CORE) $(LDFLAGS) $(LDLIBS) -o $@
build/interop_pico: tests/test_interop.cpp $(INTEROP_CORE) $(PICO_WIRE) $(wildcard $(PICO)/include/*.h) $(WIRE)
	$(CXX) $(CPPFLAGS) -I$(PICO)/include -DTEST_PICO -std=c++17 -Wall -Wextra $< $(PICO_WIRE) $(INTEROP_CORE) $(LDFLAGS) $(LDLIBS) -o $@
build/esp_%_test: $(ESP)/host_tests/%_test.cpp $(ESP)/src/%.cpp $(wildcard $(ESP)/src/lib/*.h) $(WIRE) | build
	$(CXX) -I$(ESP)/src -std=c++17 -Wall -Wextra $< $(ESP)/src/$*.cpp $(LDFLAGS) -o $@
build/test_pico: tests/test_pico.cpp $(PICO)/src/task_parser.cpp $(wildcard $(PICO)/src/scheduler/*.cpp) $(wildcard $(PICO)/include/*.h) $(wildcard $(PICO)/include/scheduler/*.h) $(WIRE) | build
	$(CXX) -I$(PICO)/include -std=c++17 -Wall -Wextra tests/test_pico.cpp $(PICO)/src/task_parser.cpp $(wildcard $(PICO)/src/scheduler/*.cpp) $(LDFLAGS) -o $@
build/pico_ut.o: $(PICO)/src/ut.c $(PICO)/include/ut.h | build
	$(CC) -I$(PICO)/include $(CFLAGS) -c $< -o $@
build/native_pico: tests/native_pico.cpp tests/native/Arduino.h tests/native/pico/time.h $(PICO_SRC) $(wildcard $(PICO)/include/*.h) $(wildcard $(PICO)/include/scheduler/*.h) build/pico_ut.o $(WIRE) | build
	$(CXX) -Itests/native -I$(PICO)/include -std=c++17 -O2 -Wall -Wextra tests/native_pico.cpp $(PICO_SRC) build/pico_ut.o $(LDFLAGS) -o $@
test: build/host build/test_core build/test_report build/interop_esp build/interop_pico build/test_pico $(ESP_TESTS) build/native_pico
	./build/test_core
	./build/test_report
	./build/interop_esp
	./build/interop_pico
	./build/test_pico
	@for test in $(ESP_TESTS); do $$test || exit 1; done
	python3 tests/integration.py
sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS='-std=c11 -O1 -g -Wall -Wextra -Wpedantic -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined' test
clean:
	rm -rf build
