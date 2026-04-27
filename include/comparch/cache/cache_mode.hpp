#pragma once

#include "comparch/cache/cache.hpp"
#include "comparch/cache/main_memory.hpp"
#include "comparch/cli.hpp"
#include "comparch/config.hpp"

namespace comparch::cache {

// Translate a JSON-driven CacheLevelConfig into the runtime Cache::Config.
// Geometry fields (size_kb, block_size, assoc) must be powers of two.
// Throws ConfigError on a malformed level.
Cache::Config to_cache_config(const CacheLevelConfig& level);

MainMemory::Config to_memory_config(const MemoryConfig& mem);

// Run --mode cache: walks the trace, drives L1 -> L2 -> DRAM, prints stats.
int run_cache_mode(const SimConfig& cfg, const CliArgs& cli);

} // namespace comparch::cache
