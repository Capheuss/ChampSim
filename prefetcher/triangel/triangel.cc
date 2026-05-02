#include "triangel.h"
#include "cache.h"

#include <algorithm>
#include <cstdio>

namespace {
constexpr uint64_t TRIANGEL_NOENTRY_PRINT_PERIOD = 500000; // rate-limit noisy debug
}

void triangel::prefetcher_initialize()
{
    state_.markov.reserve(MARKOV_MAX);
    state_.markov_fifo.reserve(MARKOV_MAX);
    dueller_init();
    std::printf("[Triangel] initialized\n");
}

uint64_t triangel::now() const {
  return static_cast<uint64_t>(
      intern_->current_time.time_since_epoch() / intern_->clock_period);
}

void triangel::prefetcher_cycle_operate() {}

uint32_t triangel::prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                      uint32_t metadata_in)
{
    static uint64_t triangel_ops = 0;
    if (++triangel_ops % 500000 == 0)
        std::printf("[Triangel] ops=%lu markov=%zu prefetches_issued=???\n", triangel_ops, state_.markov.size());
    if (type == access_type::PREFETCH) return metadata_in;

    const uint64_t cycle   = now();
    const uint64_t cl      = addr.to<uint64_t>() >> LOG2_BLOCK_SIZE;
    const uint64_t ip_val  = ip.to<uint64_t>();

    dueller_on_data_access(cl);
    dueller_maybe_resize();

    TUEntry* te = state_.get_tu(ip_val);
    if (te == nullptr) return metadata_in;

    te->local_ts++;

    const uint64_t trigger = te->lookahead ? te->cl2 : te->cl1;

    if (trigger != 0) {
        const uint16_t tu_idx = static_cast<uint16_t>(TriangelState::tu_index(ip_val));

        if (auto* scs_e = state_.scs_find(cl, tu_idx); scs_e != nullptr) {
            if (cycle <= scs_e->ready_by) {
                if (te->base_pat < CONF_MAX) ++te->base_pat;
                if (te->high_pat < CONF_MAX) ++te->high_pat;
            } else {
                te->base_pat = (te->base_pat > BASEPAT_DEC) ? static_cast<uint8_t>(te->base_pat - BASEPAT_DEC) : 0;
                te->high_pat = (te->high_pat > HIGHPAT_DEC) ? static_cast<uint8_t>(te->high_pat - HIGHPAT_DEC) : 0;
            }
            scs_e->valid = false;
        }

        const uint32_t sr = te->sample_rate;
        const uint32_t shift = (sr >= 12) ? 0u : static_cast<uint32_t>(12u - sr);
        const uint32_t denom_mask = (shift >= 31) ? 0xFFFFFFFFu : ((1u << shift) - 1u);
        const uint32_t h = static_cast<uint32_t>((trigger ^ (ip_val >> 2) ^ cycle) & 0xFFFFFFFFu);
        const bool do_sample = ((h & denom_mask) == 0);

        if (do_sample) {
            if (HSEntry* hs_e = state_.hs_find(trigger, tu_idx); hs_e != nullptr) {
                const uint32_t dist = te->local_ts - hs_e->local_ts;
                if (dist < static_cast<uint32_t>(MARKOV_MAX)) {
                    if (te->reuse_conf < CONF_MAX) ++te->reuse_conf;
                }

                if (hs_e->target == cl) {
                    if (te->base_pat < CONF_MAX) ++te->base_pat;
                    if (te->high_pat < CONF_MAX) ++te->high_pat;
                } else {
                    state_.scs_insert(trigger, hs_e->target, tu_idx, cycle);
                    te->base_pat = (te->base_pat > BASEPAT_DEC) ? static_cast<uint8_t>(te->base_pat - BASEPAT_DEC) : 0;
                    te->high_pat = (te->high_pat > HIGHPAT_DEC) ? static_cast<uint8_t>(te->high_pat - HIGHPAT_DEC) : 0;
                }

                state_.hs_insert(trigger, cl, tu_idx, te->local_ts);
            } else {
                state_.hs_insert(trigger, cl, tu_idx, te->local_ts);
            }
        }
    }

    if (te->high_pat >= CONF_INIT) te->lookahead = true;
    if (te->base_pat < CONF_STORE_MIN) te->lookahead = false;

    int degree = 0;
    get_degree(te, degree);

    // Train transition first (prev -> current) so future lookups have a chance to hit.
    if (te->cl1 != 0 && cl != te->cl1) {
        state_.markov_store(te->cl1, cl);
        state_.mrb_insert(te->cl1, cl);
    }

    // Issue prefetches for future addresses starting from *current* cl.
    // A Markov prefetcher predicts next based on current address using history from prior occurrences.
    if (degree > 0)
        issue_prefetches(cl, degree);
    te->cl2 = te->cl1;
    te->cl1 = cl;

    return metadata_in;
}


// Walk the Markov chain starting from trigger.
void triangel::issue_prefetches(uint64_t trigger, int degree)
{
    uint64_t cur = trigger;
    for (int d = 0; d < degree; ++d) {
        MarkovEntry* me = state_.markov_lookup(cur);
        if (me == nullptr) break;
        const uint64_t target = me->target;
        if (target == 0) break;
        std::printf("[Triangel] prefetch_line cl=%lu\n", target);
        intern_->prefetch_line(champsim::address{target << LOG2_BLOCK_SIZE}, true, 0);
        state_.mrb_insert(cur, target);
        cur = target;
    }
}

void triangel::dueller_init()
{
    state_.llc_sets = (intern_ != nullptr && intern_->NUM_SET != 0) ? intern_->NUM_SET : state_.llc_sets;
    state_.data_ways = (intern_ != nullptr && intern_->NUM_WAY != 0) ? intern_->NUM_WAY : state_.data_ways;
    if (state_.data_ways > DATA_WAYS_MAX) state_.data_ways = DATA_WAYS_MAX;

    state_.markov_ways = state_.data_ways / 2;
    if (state_.markov_ways > MARKOV_WAYS_MAX) state_.markov_ways = MARKOV_WAYS_MAX;

    state_.markov_step = (state_.markov_ways != 0) ? static_cast<uint32_t>((MARKOV_MAX - MARKOV_MIN) / state_.markov_ways) : 1;

    uint32_t x = 0xC0FFEEu;
    auto next = [&]() {
      x = x * 1664525u + 1013904223u; // LCG
      return x;
    };

    std::size_t filled = 0;
    while (filled < DUELLER_SETS) {
      const uint16_t s = static_cast<uint16_t>(next() % state_.llc_sets);
      bool used = false;
      for (std::size_t i = 0; i < filled; ++i) if (state_.sampled_sets[i] == s) { used = true; break; }
      if (used) continue;
      state_.sampled_sets[filled++] = s;
    }

    state_.dueller_accesses = 0;
    state_.dueller_scores.fill(0);
}

void triangel::dueller_on_data_access(uint64_t cl)
{
    const uint16_t llc_set = static_cast<uint16_t>(cl % state_.llc_sets);
    const std::size_t si = state_.sampled_set_index(llc_set);
    if (si >= DUELLER_SETS) return;

    auto& ds = state_.dueller_sets[si];
    const uint16_t tag = TriangelState::hash10(cl / state_.llc_sets);
    const std::size_t pos = TriangelState::lru_access(ds.data_tags, ds.data_valid, tag, state_.data_ways);

    // If hit at position pos (1..16), then any partition with cacheWays >= pos would hit.
    // cacheWays = data_ways - markovWays.
    if (pos != 0) {
      const int max_markov = static_cast<int>(std::min<std::size_t>(state_.markov_ways, state_.data_ways - pos));
      for (int mw = 0; mw <= max_markov; ++mw)
        state_.dueller_scores[static_cast<std::size_t>(mw)] += 1;
    }

    ++state_.dueller_accesses;
}

void triangel::dueller_on_markov_access(uint64_t trigger_cl)
{
    const uint16_t llc_set = static_cast<uint16_t>(trigger_cl % state_.llc_sets);
    const std::size_t si = state_.sampled_set_index(llc_set);
    if (si >= DUELLER_SETS) return;

    auto& ds = state_.dueller_sets[si];
    const uint16_t tag = TriangelState::hash10(trigger_cl / state_.llc_sets);
    const std::size_t pos = TriangelState::lru_access(ds.markov_tags, ds.markov_valid, tag, state_.markov_ways);

    // If hit at position pos (1..8), then any partition with markovWays >= pos would hit.
    if (pos != 0) {
      for (std::size_t mw = pos; mw <= state_.markov_ways; ++mw)
        state_.dueller_scores[mw] += MARKOV_HIT_WEIGHT;
    }
}

void triangel::dueller_maybe_resize()
{
    if (state_.dueller_accesses < DUELLER_WINDOW) return;

    // Choose best Markov-ways option.
    std::size_t best_mw = 0;
    uint32_t best_score = state_.dueller_scores[0];
    for (std::size_t mw = 1; mw <= state_.markov_ways; ++mw) {
      if (state_.dueller_scores[mw] > best_score) {
        best_score = state_.dueller_scores[mw];
        best_mw = mw;
      }
    }

    const std::size_t new_cap = MARKOV_MIN + best_mw * state_.markov_step;
    state_.markov_cap = std::min<std::size_t>(MARKOV_MAX, std::max<std::size_t>(MARKOV_MIN, new_cap));

    // Trim using markov_fifo for deterministic eviction order
    while (state_.markov.size() > state_.markov_cap && !state_.markov_fifo.empty()) {
        state_.markov.erase(state_.markov_fifo.front());
        state_.markov_fifo.erase(state_.markov_fifo.begin());
    }

    // Reset window
    state_.dueller_accesses = 0;
    state_.dueller_scores.fill(0);
}

uint32_t triangel::prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in)
{
  (void)addr; (void)set; (void)way; (void)prefetch; (void)evicted_addr;
  return metadata_in;
}

void triangel::prefetcher_final_stats()
{
    std::printf("[Triangel] final markov_size=%zu\n", state_.markov.size());
}
