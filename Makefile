SHELL := /bin/sh
BUILD ?= build
REDIS_BIN ?= ../.cache/cinder-tools/redis-7.2.4/src
SANITIZER ?=
PYTHON ?= python3
VENV := .venv
PY := $(VENV)/bin/python
FORMAT := $(VENV)/bin/clang-format
.PHONY: setup build test lint bench demo asan tsan scan container-test
setup:
	$(PYTHON) -m venv $(VENV)
	$(PY) -m pip install -r requirements-dev.txt
build:
	cmake -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=Release -DSANITIZER=$(SANITIZER)
	cmake --build $(BUILD) -j 4
lint:
	$(FORMAT) --dry-run --Werror src/*.cpp src/*.hpp tests/*.cpp
	$(VENV)/bin/ruff check scripts tests
	$(VENV)/bin/ruff format --check scripts tests
	node --check web/app.js
test: build
	./$(BUILD)/core_tests
	CINDER_BINARY=$(BUILD)/cinder CINDER_RESULTS=docs/results/$(BUILD) $(PY) -m pytest -q
bench: build
	$(PY) scripts/bench.py --redis-bin "$(REDIS_BIN)"
demo: setup build
	$(PY) scripts/demo.py
asan:
	ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 $(MAKE) test BUILD=build-asan SANITIZER=address
tsan:
	TSAN_OPTIONS=halt_on_error=1 $(MAKE) test BUILD=build-tsan SANITIZER=thread
scan:
	$(PY) -m pip_audit -r requirements-dev.txt -r requirements-browser.txt --progress-spinner off
container-test:
	docker build -t cinder-ci .
	$(PY) scripts/container_smoke.py
