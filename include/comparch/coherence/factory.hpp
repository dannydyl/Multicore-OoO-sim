#pragma once

// Per-protocol Agent factory. Steps 4-8 register agents one at a time;
// unported protocols throw at runtime so an accidental --protocol
// switch surfaces immediately rather than hanging.

#include "comparch/coherence/coherence_cache.hpp"
#include "comparch/coherence/settings.hpp"

namespace comparch::coherence {

AgentFactory make_agent_factory(Protocol p);

} // namespace comparch::coherence
