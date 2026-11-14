.DEFAULT_GOAL := all
CMAKE := $(CURDIR)/deps/cmake/bin/cmake
GUEST_CC := $(CURDIR)/deps/toolchain/bin/riscv32-pico-elf-gcc
PICOTOOL ?= $(CURDIR)/deps/picotool/picotool
GUEST_FLAGS := -march=rv32imac_zicsr_zifencei -mabi=ilp32 -Os -g -ffreestanding -fno-builtin -fno-pic -fno-pie -fno-stack-protector -Wall -Wextra -Werror -nostdlib -Isrc -Itests -Wl,-m,elf32lriscv,-z,max-page-size=4096,-T,tests/guest.ld
DEFAULT_GUEST_FLAGS := -march=rv32imac_zicsr_zifencei -mabi=ilp32 -Os -g -ffreestanding -fno-builtin -fno-pic -fno-pie -fno-stack-protector -Wall -Wextra -Werror -nostdlib -static -no-pie -Isrc -Itests -Wl,-Ttext-segment=0x20000000,-e,_start
GUEST_COMMON := tests/guest_start.S tests/guest.c tests/guest.h src/syscall.h src/memory.h tests/guest.ld Makefile
GUESTS := $(addprefix build/,$(addsuffix .elf,hello echo loop exit23 registers exec assembly rwx threads threads_busy threads_fault))
PROBES := $(addprefix build/probe,$(addsuffix .elf,1 2 3 4 5 6 7 8 9 10 11 12))
NATIVE_SOURCES := memory launch map elf wire loader program transfer
C_FILES := $(wildcard src/*.c src/*.h tests/*.c tests/*.h)
PY_FILES := $(wildcard src/*.py scripts/*.py tests/*.py)
VERSION := $(strip $(file <VERSION))
RELEASE_BUNDLE := dist/pico2monitor-$(VERSION).tar.gz

.PHONY: all deps firmware guests emulator check native-check emulator-check panic-check format format-check flash install-udev clean
all: dist/monitor.uf2 dist/pico guests build/emulator
.PHONY: release
release: $(RELEASE_BUNDLE)
deps:
	./scripts/setup-deps.py

build dist:
	mkdir -p $@

# Dependency setup checks each installed component instead of trusting a stamp.
build/CMakeCache.txt: CMakeLists.txt | deps
	$(CMAKE) -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
firmware: build/CMakeCache.txt
	$(CMAKE) --build build
build/monitor.uf2 build/monitor_emu.hex &: firmware
	@test -f $@
dist/monitor.uf2: build/monitor.uf2 | dist
	cp $< $@
dist/pico: src/pico.py | dist
	cp $< $@

# Package the protected firmware with the end-user quickstart.
dist/README.md: RELEASE.md | dist
	cp $< $@
$(RELEASE_BUNDLE): dist/monitor.uf2 dist/pico dist/README.md VERSION Makefile
	tar -czf $@ -C dist monitor.uf2 pico README.md

guests: $(GUESTS) $(PROBES)
build/hello.elf build/echo.elf build/loop.elf build/exit23.elf: tests/guest_demo.c $(GUEST_COMMON) | build deps
	$(GUEST_CC) $(GUEST_FLAGS) -DDEMO=$(if $(findstring hello,$@),1,$(if $(findstring echo,$@),2,$(if $(findstring loop,$@),3,4))) tests/guest_start.S tests/guest.c $< -o $@
build/probe%.elf: tests/guest_probe.c $(GUEST_COMMON) | build deps
	$(GUEST_CC) $(GUEST_FLAGS) -DPROBE=$* tests/guest_start.S tests/guest.c $< -o $@
build/registers.elf: tests/guest_registers.S tests/guest.ld Makefile | build deps
	$(GUEST_CC) $(GUEST_FLAGS) $< -o $@
build/exec.elf: tests/guest_exec.c $(GUEST_COMMON) | build deps
	$(GUEST_CC) $(DEFAULT_GUEST_FLAGS) tests/guest_start.S tests/guest.c $< -o $@
build/assembly.elf: tests/hello.S Makefile | build deps
	$(GUEST_CC) $(DEFAULT_GUEST_FLAGS) $< -o $@
build/rwx.elf: tests/guest_rwx.S Makefile | build deps
	$(GUEST_CC) $(DEFAULT_GUEST_FLAGS) -Wl,-N $< -o $@
build/threads.elf build/threads_busy.elf build/threads_fault.elf: tests/guest_threads.S src/syscall.h Makefile | build deps
	$(GUEST_CC) $(DEFAULT_GUEST_FLAGS) -DTHREAD_TEST=$(if $(findstring busy,$@),1,$(if $(findstring fault,$@),2,0)) $< -o $@
deps/rp2350js-c.h: | deps
	@test -f $@
build/emulator: tests/emulator.c deps/rp2350js-c.h | build
	$(CC) -O2 -std=gnu11 -Ideps $< -o $@ -lm
build/test-panic: tests/test_panic.c deps/rp2350js-c.h | build
	$(CC) -O2 -std=gnu11 -Ideps $< -o $@ -lm
panic-check: build/test-panic build/monitor_emu.hex
	PYTHONPATH=src:tests python3 -m unittest -v test_panic
emulator: build/emulator build/monitor_emu.hex
	./build/emulator build/monitor_emu.hex

build/test-native: tests/test_native.c $(addprefix src/,$(addsuffix .c,$(NATIVE_SOURCES))) $(wildcard src/*.h) | build
	$(CC) -std=c11 -O1 -g -Wall -Wextra -Werror -Wshadow -fsanitize=address,undefined -DMONITOR_NATIVE_TEST -Isrc $< $(addprefix src/,$(addsuffix .c,$(NATIVE_SOURCES))) -o $@
native-check: build/test-native guests
	./build/test-native build/hello.elf
emulator-check: build/emulator build/monitor_emu.hex guests
	PYTHONPATH=src:tests python3 -m unittest -v test_emulator
format:
	deps/format-tools/usr/bin/clang-format-19 -i $(C_FILES)
	uv run ruff format $(PY_FILES)
format-check:
	deps/format-tools/usr/bin/clang-format-19 --dry-run --Werror $(C_FILES)
	uv run ruff format --check $(PY_FILES)
	uv run ruff check $(PY_FILES)
	uv run ty check
format format-check: deps/format-tools/usr/bin/clang-format-19
deps/format-tools/usr/bin/clang-format-19:
	mkdir -p deps/format-tools
	cd deps/format-tools && apt-get download clang-format-19
	dpkg-deb -x deps/format-tools/clang-format-19_*.deb deps/format-tools
check: format-check
	$(MAKE) native-check
	PYTHONPATH=src:tests python3 -m unittest -v test_host
	$(MAKE) emulator-check
	$(MAKE) panic-check
flash: dist/monitor.uf2
	sudo $(PICOTOOL) load -v $<
	sudo $(PICOTOOL) reboot
install-udev: 60-pico-monitor.rules
	sudo install -m 644 $< /etc/udev/rules.d/60-pico-monitor.rules
	sudo udevadm control --reload-rules
clean:
	rm -rf build dist
