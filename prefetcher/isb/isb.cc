#include "isb.h"
#include "cache.h"
#include <algorithm>
#include <cstdio>

namespace {
constexpr uint64_t ISB_DEBUG_PERIOD = 200'000;
}

void isb::prefetcher_cycle_operate()
{
    const uint64_t now = static_cast<uint64_t>(
        intern_->current_time.time_since_epoch() / intern_->clock_period);

    auto& pend = state_.pending;
    pend.erase(std::remove_if(pend.begin(), pend.end(), [&](const ISBPending& p) {
        if (p.ready <= now) {
            const bool ok = prefetch_line(champsim::address{p.pf_cl << 6}, true, 0);
            if (ok) ++state_.dbg_prefetch_accepted;
            else    ++state_.dbg_prefetch_rejected;
            return true;
        }
        return false;
    }), pend.end());
}

uint32_t isb::prefetcher_cache_operate(champsim::address addr, champsim::address ip,
                                       uint8_t cache_hit, bool useful_prefetch,
                                       access_type type, uint32_t metadata_in)
{
    if (type == access_type::PREFETCH) return metadata_in;

    const uint64_t now = static_cast<uint64_t>(
        intern_->current_time.time_since_epoch() / intern_->clock_period);
    const uint64_t cl = addr.to<uint64_t>() >> 6;
    uint64_t& last_cl = state_.last_cl_per_ip[ip.to<uint64_t>()];

    ++state_.dbg_ops;
    if (useful_prefetch) {
        ++state_.dbg_useful;
        std::printf("[ISB] useful prefetch at cl=%lu\n", cl);
    }

    if (last_cl != 0 && last_cl != cl) {
        ++state_.dbg_train_steps;
        auto [prev_sa_opt, _1] = state_.ps_get(last_cl);

        if (!prev_sa_opt.has_value()) {
            const uint64_t sa0 = state_.next_sa++;
            ++state_.dbg_new_streams;
            state_.ps_set(last_cl, sa0);
            state_.sp_set(sa0, last_cl);
            prev_sa_opt = sa0;
        }

        const uint64_t expected_sa = *prev_sa_opt + 1;
        auto [curr_sa_opt, _2] = state_.ps_get(cl);

        if (curr_sa_opt.has_value()) {
            if (*curr_sa_opt == expected_sa) {
                state_.inc_conf(last_cl);
                state_.inc_conf(cl);
            } else {
                state_.dec_conf(last_cl);
                state_.dec_conf(cl);
            }
        } else {
            state_.ps_set(cl, expected_sa);
            state_.sp_set(expected_sa, cl);
        }
    }

    last_cl = cl;

    if (!cache_hit)
        do_prefetch(cl, now);

    if ((state_.dbg_ops % ISB_DEBUG_PERIOD) == 0) {
        std::printf(
            "[ISB] dbg ops=%lu useful=%lu new_streams=%lu conf_inc=%lu conf_dec=%lu "
            "ps_hit=%lu ps_miss=%lu sp_hit=%lu sp_miss=%lu "
            "pf_attempts=%lu ok=%lu rej=%lu\n",
            state_.dbg_ops, state_.dbg_useful, state_.dbg_new_streams,
            state_.dbg_conf_inc, state_.dbg_conf_dec,
            state_.dbg_ps_hits, state_.dbg_ps_misses,
            state_.dbg_sp_hits, state_.dbg_sp_misses,
            state_.dbg_prefetch_attempts,
            state_.dbg_prefetch_accepted, state_.dbg_prefetch_rejected);
    }

    return metadata_in;
}

void isb::do_prefetch(uint64_t trigger_cl, uint64_t now)
{
    auto [sa_opt, ps_offchip] = state_.ps_get(trigger_cl);
    if (!sa_opt.has_value()) return;
    if (state_.conf(trigger_cl) < ISB_CONF_THRESH) return;

    for (int d = 1; d <= ISB_DEGREE; ++d) {
        auto [pf_cl_opt, sp_offchip] = state_.sp_get(*sa_opt + static_cast<uint64_t>(d));
        if (!pf_cl_opt.has_value()) continue;
        if (*pf_cl_opt == trigger_cl) continue;
        ++state_.dbg_prefetch_attempts;

        if (ps_offchip || sp_offchip) {
            if (state_.pending.size() < ISB_PENDING_MAX) {
                state_.pending.push_back({*pf_cl_opt, now + ISB_OFFCHIP_LAT});
                ++state_.dbg_prefetch_delayed;
            } else {
                ++state_.dbg_pending_dropped;
            }
        } else {
            const bool ok = prefetch_line(champsim::address{*pf_cl_opt << 6}, true, 0);
            if (ok) ++state_.dbg_prefetch_accepted;
            else    ++state_.dbg_prefetch_rejected;
            ++state_.dbg_prefetch_immediate;
        }
    }
}

uint32_t isb::prefetcher_cache_fill(champsim::address addr, long set, long way,
                                    uint8_t prefetch, champsim::address evicted_addr,
                                    uint32_t metadata_in)
{
    return metadata_in;
}
