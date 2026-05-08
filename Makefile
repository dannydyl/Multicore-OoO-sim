# Validation sweep harness.
#
# Common entry points: `make smoke`, `make short`, `make medium`, `make long`.
# Each composes: build (rebuild release if stale) -> traces (gen_synth + fetch)
# -> sweep -> aggregate. Variables override on the command line:
#
#   make short SWEEP_ID=v3 JOBS=4 TIMEOUT=900
#   make sweep TIER=long SWEEP_ID=overnight SWEEP_CFG=configs/my_sweep.json
#   make clean SWEEP_ID=v3
#
# `make clean` removes one sweep's artifacts; `make clean-all` nukes
# everything under report/.

PYTHON ?= python3
TIER ?= smoke
SWEEP_ID ?= $(TIER)
JOBS ?= 8
TIMEOUT ?= 1800
SWEEP_CFG ?= configs/sweep.json

# Single-run knobs for `make run`. Lowercase aliases let users type
# `make run trace=...` instead of `TRACE=...`.
CONFIG   ?= configs/baseline.json
TRACE    ?= $(trace)
TAG      ?= $(tag)
PROTOCOL ?= $(protocol)
CORES    ?= $(cores)

# Compile-time knob (override on command line, e.g. make build FAST=1).
# Maps to the CASIM_FAST option in CMakeLists.txt (-O2).
FAST ?= 0

SCRIPTS := scripts
BUILD_DIR := build-release
SIM_BIN := $(BUILD_DIR)/src/sim
GEN_TRACE_BIN := $(BUILD_DIR)/tools/gen_trace/gen_trace
REPORT_DIR := report

# 1/0/y/n -> ON/OFF for cmake
bool = $(if $(filter 1 ON on YES yes y true,$1),ON,OFF)

CMAKE_FLAGS := -DCASIM_FAST=$(call bool,$(FAST))

# Non-empty when FAST was explicitly passed on the command line — in that
# case `build` reconfigures cmake. Otherwise it keeps the existing
# skip-if-present fast path so `make sweep` doesn't re-run cmake every time.
OPT_OVERRIDDEN := $(if $(filter command line,$(origin FAST)),1)

.PHONY: help build run traces gen-synth fetch-traces \
        smoke short medium long sweep aggregate \
        clean clean-all clean-reports clean-traces \
        list dry-run

define HELP_TEXT

    __  ___      ____  _    _____ _
   /  |/  /_  __/ / /_(_)  / ___/(_)___ ___
  / /|_/ / / / / / __/ /   \__ \/ / __ `__ \ 
 / /  / / /_/ / / /_/ /   ___/ / / / / / / /
/_/  /_/\__,_/_/\__/_/   /____/_/_/ /_/ /_/
   Out-of-Order Multicore  ·  sweep harness
   build ─▶ traces ─▶ sweep ─▶ aggregate

  First time?   make smoke      (~1 min, tiny synth)
  Quick check?  make short      (~10 min, +champsim)
  Full run?     make long       (overnight, 100M synth)

USAGE
  make <target> [VAR=value ...]

TARGETS
  run        TRACE=<dir|file>       run one simulation (auto-detects cores)
  smoke | short | medium | long     end-to-end (build+traces+sweep+aggregate)
  build                             cmake build only
  traces            [TIER=<t>]      gen-synth + fetch-traces
  sweep      TIER=<t> SWEEP_ID=<id> run sweep (no aggregate)
  aggregate  SWEEP_ID=<id>          parse + write summary.md
  dry-run    TIER=<t>               list runs without executing
  list                              show configured tiers
  clean      SWEEP_ID=<id>          remove one sweep's artifacts
  clean-reports                     remove EVERYTHING under report/ (sweeps + manual runs)
  clean-all                         alias for clean-reports
  clean-traces                      remove traces/synth + traces/champsim

VARIABLES (override on command line)
  TIER=smoke   SWEEP_ID=smoke   JOBS=8   TIMEOUT=1800
  SWEEP_CFG=configs/sweep.json

  Single-run vars (for `make run`; lowercase aliases also accepted):
  TRACE=<dir|file>  CONFIG=configs/baseline.json
  CORES=<n>         TAG=<name>          PROTOCOL=<mi|msi|mesi|mosi|moesif>

BUILD OPTIONS (override on `make build`; reconfigures cmake)
  FAST=1       -O2 optimization

EXAMPLES
  $$ make smoke
  $$ make run TRACE=traces/core_4
  $$ make run TRACE=traces/synth/random_small TAG=v1 PROTOCOL=mosi
  $$ make run trace=traces/mix.txt config=configs/baseline.json
  $$ make build FAST=1
  $$ make short SWEEP_ID=v3 JOBS=4
  $$ make sweep TIER=long SWEEP_ID=overnight
  $$ make clean SWEEP_ID=v3

endef
export HELP_TEXT

help:
	@printf '%s\n' "$$HELP_TEXT"
	@printf 'Current: TIER=%s  SWEEP_ID=%s  JOBS=%s  TIMEOUT=%s  SWEEP_CFG=%s\n\n' \
	    '$(TIER)' '$(SWEEP_ID)' '$(JOBS)' '$(TIMEOUT)' '$(SWEEP_CFG)'

build:
ifdef OPT_OVERRIDDEN
	@echo "==> cmake configure $(BUILD_DIR) (FAST=$(FAST))"
	@cmake -S . -B $(BUILD_DIR) $(CMAKE_FLAGS)
	@echo "==> cmake --build $(BUILD_DIR) -j"
	@cmake --build $(BUILD_DIR) -j
else
	@if [ ! -x $(SIM_BIN) ] || [ ! -x $(GEN_TRACE_BIN) ]; then \
		echo "==> cmake --build $(BUILD_DIR)"; \
		cmake --build $(BUILD_DIR) -j; \
	else \
		echo "==> binaries present (skip build); pass FAST=1 to reconfigure"; \
	fi
endif

# Run a single simulation against one trace source.
#
#   make run TRACE=traces/core_4
#   make run TRACE=traces/synth/random_small TAG=v1 PROTOCOL=mosi
#   make run trace=traces/mix.txt   config=configs/baseline.json
#
# When TRACE is a directory we pass --trace-dir and auto-set --cores to
# the number of p*.champsimtrace files inside (unless CORES is overridden).
# When TRACE is a regular file we pass --trace-list. Defaults: CONFIG =
# configs/baseline.json. Lowercase variable forms (trace, tag, cores,
# protocol, config) are accepted as aliases.
run: build
	@if [ -z "$(TRACE)" ]; then \
	    echo "ERROR: TRACE not set."; \
	    echo "Usage:  make run TRACE=<traces/dir-or-manifest> [CONFIG=...] [CORES=N] [TAG=name] [PROTOCOL=mi|msi|mesi|mosi|moesif]"; \
	    exit 1; \
	fi
	@cores_arg=""; tag_arg=""; proto_arg=""; flag=""; \
	if [ -d "$(TRACE)" ]; then \
	    flag="--trace-dir"; \
	    if [ -z "$(CORES)" ]; then \
	        n=$$(ls "$(TRACE)"/p*.champsimtrace 2>/dev/null | wc -l | tr -d ' '); \
	        if [ "$$n" -gt 0 ]; then cores_arg="--cores $$n"; fi; \
	    fi; \
	elif [ -f "$(TRACE)" ]; then \
	    flag="--trace-list"; \
	else \
	    echo "ERROR: TRACE='$(TRACE)' is neither a directory nor a file"; \
	    exit 2; \
	fi; \
	if [ -n "$(CORES)" ]; then cores_arg="--cores $(CORES)"; fi; \
	if [ -n "$(TAG)" ]; then tag_arg="--tag $(TAG)"; fi; \
	if [ -n "$(PROTOCOL)" ]; then proto_arg="--protocol $(PROTOCOL)"; fi; \
	echo "==> $(SIM_BIN) --config $(CONFIG) $$flag $(TRACE) $$cores_arg $$tag_arg $$proto_arg"; \
	$(SIM_BIN) --config $(CONFIG) $$flag $(TRACE) $$cores_arg $$tag_arg $$proto_arg

gen-synth: build
	@echo "==> gen_synth --tier $(TIER) (--sweep-config $(SWEEP_CFG))"
	@$(PYTHON) $(SCRIPTS)/gen_synth.py --tier $(TIER) --sweep-config $(SWEEP_CFG)

fetch-traces:
	@echo "==> fetch_traces.sh"
	@bash $(SCRIPTS)/fetch_traces.sh || \
	  echo "WARN: fetch_traces.sh exited non-zero; sweep will skip missing dirs"

traces: gen-synth fetch-traces

sweep: build
	@echo "==> run_sweep.py --tier $(TIER) --sweep-id $(SWEEP_ID) --jobs $(JOBS) --timeout $(TIMEOUT)"
	@$(PYTHON) $(SCRIPTS)/run_sweep.py \
	    --tier $(TIER) --sweep-id $(SWEEP_ID) \
	    --jobs $(JOBS) --timeout $(TIMEOUT) \
	    --sweep-config $(SWEEP_CFG) $(EXTRA)

aggregate:
	@echo "==> aggregate.py --sweep-id $(SWEEP_ID)"
	@$(PYTHON) $(SCRIPTS)/aggregate.py --sweep-id $(SWEEP_ID)

dry-run: build
	@$(PYTHON) $(SCRIPTS)/run_sweep.py \
	    --tier $(TIER) --sweep-id $(SWEEP_ID) \
	    --jobs $(JOBS) --timeout $(TIMEOUT) \
	    --sweep-config $(SWEEP_CFG) --dry-run $(EXTRA)

list:
	@$(PYTHON) -c "import sys; sys.path.insert(0, '$(SCRIPTS)'); \
	 from sweep_matrix import load_matrix; \
	 m = load_matrix('$(SWEEP_CFG)'); \
	 print('Tiers in $(SWEEP_CFG):'); \
	 [print(f'  {k:<8} synth={v.get(\"synth_sizes\",[])} champsim={v.get(\"champsim\",[])} axes={v.get(\"axes\",[])} hp={v.get(\"handpicked\",False)}') for k,v in m.tiers.items()]"

# Tier shortcuts: end-to-end (build, traces, sweep, aggregate).
# Sweep failures are non-fatal so aggregate always runs and writes summary.md
# even when individual runs crashed/deadlocked. Inspect summary.md for the
# violation list.
smoke:
	@$(MAKE) traces TIER=smoke
	-@$(MAKE) sweep TIER=smoke SWEEP_ID=$(SWEEP_ID)
	@$(MAKE) aggregate SWEEP_ID=$(SWEEP_ID)

short:
	@$(MAKE) traces TIER=short
	-@$(MAKE) sweep TIER=short SWEEP_ID=$(SWEEP_ID)
	@$(MAKE) aggregate SWEEP_ID=$(SWEEP_ID)

medium:
	@$(MAKE) traces TIER=medium
	-@$(MAKE) sweep TIER=medium SWEEP_ID=$(SWEEP_ID)
	@$(MAKE) aggregate SWEEP_ID=$(SWEEP_ID)

long:
	@$(MAKE) traces TIER=long
	-@$(MAKE) sweep TIER=long SWEEP_ID=$(SWEEP_ID)
	@$(MAKE) aggregate SWEEP_ID=$(SWEEP_ID)

# Cleanup. SWEEP_ID-scoped to avoid wiping more than asked.
clean:
	@if [ -z "$(SWEEP_ID)" ] || [ "$(SWEEP_ID)" = "all" ]; then \
		echo "ERROR: refusing to clean without explicit SWEEP_ID (got '$(SWEEP_ID)'); use 'make clean-all' to nuke everything"; \
		exit 1; \
	fi
	@echo "==> cleaning sweep '$(SWEEP_ID)'"
	@rm -rf $(REPORT_DIR)/_sweep/$(SWEEP_ID)
	@for d in $(REPORT_DIR)/*_$(SWEEP_ID)__*; do \
		if [ -d "$$d" ]; then echo "  rm $$d"; rm -rf "$$d"; fi; \
	done
	@echo "done."

clean-all: clean-reports

# Remove every report directory: sweep aggregations (report/_sweep/*),
# sweep per-run dirs (report/*__*), and manual single-run dirs (report/*
# without the __ separator). Keeps the report/ directory itself.
clean-reports:
	@echo "==> removing all reports under $(REPORT_DIR)/"
	@if [ -d $(REPORT_DIR) ]; then \
		find $(REPORT_DIR) -mindepth 1 -maxdepth 1 -exec rm -rf {} +; \
	fi
	@echo "done."

clean-traces:
	@echo "==> removing traces/synth and traces/champsim"
	@rm -rf traces/synth traces/champsim
	@echo "done."
