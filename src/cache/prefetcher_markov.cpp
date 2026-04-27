#include "comparch/cache/prefetcher_markov.hpp"

#include "comparch/cache/cache.hpp"

namespace comparch::cache {

MarkovPrefetcher::MarkovPrefetcher(unsigned max_rows)
    : MAX_MKV_rows(max_rows) {}

// Mirror of project1's markov_row_sort.
void MarkovPrefetcher::markov_row_sort(std::list<markov_t>& row) {
    if (row.size() <= 1) {
        return;
    }

    auto block = row.begin();

    while (block != row.end()) {
        auto best = block;
        auto temp = block;

        ++temp;
        for (; temp != row.end(); ++temp) {
            if ((temp->num_access > best->num_access) ||
                (temp->num_access == best->num_access &&
                 temp->destination_b_addr > best->destination_b_addr)) {
                best = temp;
            }
        }

        if (best != block) {
            row.splice(block, row, best);
        }

        ++block;
    }
}

// Mirror of project1's get_MFU_block_addr.
std::uint64_t MarkovPrefetcher::get_MFU_block_addr(std::list<markov_t>& row) {
    auto block = row.begin();
    std::uint64_t best_count    = block->num_access;
    std::uint64_t MFU_block_addr = block->destination_b_addr;
    ++block;

    for (; block != row.end(); ++block) {
        if (block->num_access > best_count) {
            best_count     = block->num_access;
            MFU_block_addr = block->destination_b_addr;
        } else if ((block->num_access == best_count) &&
                   (block->destination_b_addr > MFU_block_addr)) {
            MFU_block_addr = block->destination_b_addr;
        }
    }

    return MFU_block_addr;
}

// Mirror of project1's is_dest_in_MRK_row.
bool MarkovPrefetcher::is_dest_in_MRK_row(std::list<markov_t>& row,
                                          std::uint64_t missed_addr) {
    for (auto block = row.begin(); block != row.end(); ++block) {
        if (block->destination_b_addr == missed_addr) {
            ++block->num_access;
            markov_row_sort(row);
            return true;
        }
    }
    return false;
}

// Mirror of project1's add_new_entry.
void MarkovPrefetcher::add_new_entry(std::list<markov_t>& row,
                                     std::uint64_t missed_addr) {
    if (row.size() == 4) {
        // Evict LFU. Tie-break: smallest destination_b_addr loses.
        auto block          = row.begin();
        std::uint64_t lowest_count = block->num_access;
        std::uint64_t LFU_block_addr = block->destination_b_addr;
        auto victim_row     = row.begin();

        for (; block != row.end(); ++block) {
            if (block->num_access < lowest_count) {
                lowest_count    = block->num_access;
                LFU_block_addr  = block->destination_b_addr;
                victim_row      = block;
            } else if ((block->num_access == lowest_count) &&
                       (block->destination_b_addr < LFU_block_addr)) {
                LFU_block_addr = block->destination_b_addr;
                victim_row     = block;
            }
        }
        row.erase(victim_row);
    }

    markov_t new_entry;
    new_entry.destination_b_addr = missed_addr;
    new_entry.num_access         = 1;
    row.push_front(new_entry);

    markov_row_sort(row);
}

// Mirror of project1's add_new_row.
void MarkovPrefetcher::add_new_row(std::uint64_t missed_addr) {
    if (MAX_MKV_rows == 0) {
        return;
    }
    if (MKV_col.size() == MAX_MKV_rows) {
        MKV_col.pop_back(); // evict LRU row
    }

    markov_row new_row;
    new_row.source_b_addr = prev_block;

    add_new_entry(new_row.MKV_row, missed_addr);
    MKV_col.push_front(new_row);
}

void MarkovPrefetcher::learn(std::uint64_t missed_block_addr) {
    bool prev_block_found_in_table = false;

    for (auto row = MKV_col.begin(); row != MKV_col.end(); ++row) {
        if (row->source_b_addr == prev_block) {
            prev_block_found_in_table = true;
            if (!is_dest_in_MRK_row(row->MKV_row, missed_block_addr)) {
                add_new_entry(row->MKV_row, missed_block_addr);
            }
            MKV_col.splice(MKV_col.begin(), MKV_col, row);
            break;
        }
    }

    if (!prev_block_found_in_table) {
        add_new_row(missed_block_addr);
    }
}

// Mirror of project1's markov_prefetecher.
void MarkovPrefetcher::on_miss(Cache& cache, Cache* peer,
                               const MemReq& demand) {
    const std::uint64_t missed_block_addr = cache.get_block_addr(demand.addr);

    if (MAX_MKV_rows == 0) {
        pf_first_block = false;
        prev_block     = missed_block_addr;
        return;
    }

    if (pf_first_block) {
        pf_first_block = false;
        prev_block     = missed_block_addr;
        return;
    }

    // ---- prefetch issue ----
    std::uint64_t MFU_block = 0;
    bool MKV_table_HIT = false;

    for (auto row = MKV_col.begin(); row != MKV_col.end(); ++row) {
        if (row->source_b_addr == missed_block_addr) {
            MKV_table_HIT = true;
            if (row->MKV_row.empty()) {
                MKV_table_HIT = false;
                break;
            }
            MFU_block = get_MFU_block_addr(row->MKV_row);
            break;
        }
    }

    if (MKV_table_HIT) {
        const std::uint64_t MFU_byte_addr = MFU_block << cache.cfg().b;
        const bool in_peer = peer ? peer->block_in(MFU_byte_addr) : false;
        const bool in_self = cache.block_in(MFU_byte_addr);
        if (!in_peer && !in_self && MFU_block != missed_block_addr) {
            cache.issue_prefetch(MFU_byte_addr);
        }
    }

    // ---- table learning ----
    learn(missed_block_addr);

    prev_block = missed_block_addr;
}

} // namespace comparch::cache
