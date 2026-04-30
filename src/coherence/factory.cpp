#include "comparch/coherence/factory.hpp"

#include <stdexcept>

#include "comparch/coherence/agent_mesi.hpp"
#include "comparch/coherence/agent_mi.hpp"
#include "comparch/coherence/agent_moesif.hpp"
#include "comparch/coherence/agent_mosi.hpp"
#include "comparch/coherence/agent_msi.hpp"

namespace comparch::coherence {

AgentFactory make_agent_factory(Protocol p) {
    switch (p) {
        case Protocol::MI:
            return [](NodeId id, Cache* cache, BlockId block) {
                return std::unique_ptr<Agent>(new MiAgent(id, cache, block));
            };
        case Protocol::MSI:
            return [](NodeId id, Cache* cache, BlockId block) {
                return std::unique_ptr<Agent>(new MsiAgent(id, cache, block));
            };
        case Protocol::MESI:
            return [](NodeId id, Cache* cache, BlockId block) {
                return std::unique_ptr<Agent>(new MesiAgent(id, cache, block));
            };
        case Protocol::MOSI:
            return [](NodeId id, Cache* cache, BlockId block) {
                return std::unique_ptr<Agent>(new MosiAgent(id, cache, block));
            };
        case Protocol::MOESIF:
            return [](NodeId id, Cache* cache, BlockId block) {
                return std::unique_ptr<Agent>(new MoesifAgent(id, cache, block));
            };
    }
    throw std::runtime_error("unknown coherence protocol");
}

} // namespace comparch::coherence
