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

SCRIPTS := scripts
BUILD_DIR := build-release
SIM_BIN := $(BUILD_DIR)/src/sim
GEN_TRACE_BIN := $(BUILD_DIR)/tools/gen_trace/gen_trace
REPORT_DIR := report

.PHONY: help build traces gen-synth fetch-traces \
        smoke short medium long sweep aggregate \
        clean clean-all clean-traces \
        list dry-run

help:
	@echo "Sweep harness"
	@echo ""
	@echo "Tier shortcuts (build + traces + sweep + aggregate):"
	@echo "  make smoke      [SWEEP_ID=$(SWEEP_ID)]   tiny synth, proto axis only"
	@echo "  make short      [SWEEP_ID=...]           tiny+small + champsim, all axes"
	@echo "  make medium     [SWEEP_ID=...]           up to 10M synth, 2 champsim"
	@echo "  make long       [SWEEP_ID=...]           up to 100M synth, 3 champsim"
	@echo ""
	@echo "Building blocks:"
	@echo "  make build                         cmake --build $(BUILD_DIR) -j"
	@echo "  make traces TIER=<t>               gen_synth.py + fetch_traces.sh"
	@echo "  make gen-synth TIER=<t>            synth traces only"
	@echo "  make fetch-traces                  download ChampSim public traces"
	@echo "  make sweep TIER=<t> SWEEP_ID=<id>  run sweep without aggregate"
	@echo "  make aggregate SWEEP_ID=<id>       parse + summarize a sweep"
	@echo "  make dry-run TIER=<t>              list runs without executing"
	@echo "  make list                          list configured tiers"
	@echo ""
	@echo "Cleanup:"
	@echo "  make clean SWEEP_ID=<id>           rm one sweep's artifacts"
	@echo "  make clean-all CONFIRM=y           rm every sweep + per-run dir"
	@echo "  make clean-traces CONFIRM=y        rm generated synth + champsim trace dirs"
	@echo ""
	@echo "Variables (override with VAR=...):"
	@echo "  TIER=$(TIER)  SWEEP_ID=$(SWEEP_ID)  JOBS=$(JOBS)  TIMEOUT=$(TIMEOUT)  SWEEP_CFG=$(SWEEP_CFG)"

build:
	@if [ ! -x $(SIM_BIN) ] || [ ! -x $(GEN_TRACE_BIN) ]; then \
		echo "==> cmake --build $(BUILD_DIR)"; \
		cmake --build $(BUILD_DIR) -j; \
	else \
		echo "==> binaries present (skip build); run 'cmake --build $(BUILD_DIR) -j' to refresh"; \
	fi

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
		echo "ERROR: refusing to clean without explicit SWEEP_ID (got '$(SWEEP_ID)'); use 'make clean-all CONFIRM=y' to nuke everything"; \
		exit 1; \
	fi
	@echo "==> cleaning sweep '$(SWEEP_ID)'"
	@rm -rf $(REPORT_DIR)/_sweep/$(SWEEP_ID)
	@for d in $(REPORT_DIR)/*_$(SWEEP_ID)__*; do \
		if [ -d "$$d" ]; then echo "  rm $$d"; rm -rf "$$d"; fi; \
	done
	@echo "done."

clean-all:
	@if [ "$(CONFIRM)" != "y" ]; then \
		echo "About to remove ALL sweep artifacts under $(REPORT_DIR)/:"; \
		ls -d $(REPORT_DIR)/_sweep $(REPORT_DIR)/*__* 2>/dev/null || echo "  (nothing to clean)"; \
		echo ""; \
		echo "Re-run with CONFIRM=y to proceed:  make clean-all CONFIRM=y"; \
		exit 1; \
	fi
	@echo "==> removing all sweep artifacts"
	@rm -rf $(REPORT_DIR)/_sweep
	@for d in $(REPORT_DIR)/*__*; do \
		if [ -d "$$d" ]; then rm -rf "$$d"; fi; \
	done
	@echo "done."

clean-traces:
	@if [ "$(CONFIRM)" != "y" ]; then \
		echo "About to remove generated traces:"; \
		echo "  traces/synth/   (regen with 'make gen-synth')"; \
		echo "  traces/champsim/ (re-fetch with 'make fetch-traces')"; \
		echo ""; \
		echo "Re-run with CONFIRM=y to proceed:  make clean-traces CONFIRM=y"; \
		exit 1; \
	fi
	@rm -rf traces/synth traces/champsim
	@echo "done."
