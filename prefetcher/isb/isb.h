#ifndef PREFETCHER_ISB_H
#define PREFETCHER_ISB_H

#include <cstdint>
#include <array>
#include <unordered_map>
#include <vector>
#include <optional>

#include "champsim.h"
#include "modules.h"

constexpr std::size_t ISB_PS_SIZE      = 256;
constexpr std::size_t ISB_SP_SIZE      = 256;
constexpr int         ISB_DEGREE       = 2;
constexpr uint8_t     ISB_CONF_MAX     = 3;
constexpr uint8_t     ISB_CONF_THRESH  = 1;
constexpr uint64_t    ISB_OFFCHIP_LAT  = 200;
constexpr std::size_t ISB_PENDING_MAX  = 32;
constexpr std::size_t ISB_DRAM_MAX     = 4096;

template <typename K, typename V, std::size_t N>
struct ISBLRUCache {
    struct Entry { K key{}; V val{}; uint64_t ts = 0; bool valid = false; };
    std::array<Entry, N> table{};
    uint64_t clock = 0;

    V* find(const K& k) {
        for (auto& e : table)
            if (e.valid && e.key == k) { e.ts = ++clock; return &e.val; }
        return nullptr;
    }
    void insert(const K& k, const V& v) {
        for (auto& e : table)
            if (e.valid && e.key == k) { e.val = v; e.ts = ++clock; return; }
        Entry* vic = nullptr;
        for (auto& e : table)
            if (!e.valid) { vic = &e; break; }
            else if (!vic || e.ts < vic->ts) vic = &e;
        *vic = {k, v, ++clock, true};
    }
};

struct ISBPending {
  uint64_t pf_cl;
  uint64_t ready;
};

struct ISBState {
  uint64_t next_sa = 64;
  std::unordered_map<uint64_t, uint64_t> last_cl_per_ip{};  // per-IP last cache line

  ISBLRUCache<uint64_t, uint64_t, ISB_PS_SIZE> ps_chip{};
  ISBLRUCache<uint64_t, uint64_t, ISB_SP_SIZE> sp_chip{};
  std::unordered_map<uint64_t, uint64_t>        ps_dram{};
  std::unordered_map<uint64_t, uint64_t>        sp_dram{};
  std::unordered_map<uint64_t, uint8_t>         ps_conf{};

  std::vector<ISBPending> pending{};

  uint64_t dbg_ops = 0;
  uint64_t dbg_train_steps = 0;
  uint64_t dbg_new_streams = 0;
  uint64_t dbg_ps_hits = 0;
  uint64_t dbg_ps_misses = 0;
  uint64_t dbg_sp_hits = 0;
  uint64_t dbg_sp_misses = 0;
  uint64_t dbg_prefetch_attempts = 0;
  uint64_t dbg_prefetch_immediate = 0;
  uint64_t dbg_prefetch_delayed = 0;
  uint64_t dbg_prefetch_accepted = 0;
  uint64_t dbg_prefetch_rejected = 0;
  uint64_t dbg_pending_dropped = 0;
  uint64_t dbg_useful = 0;
  uint64_t dbg_conf_inc = 0;
  uint64_t dbg_conf_dec = 0;

  std::pair<std::optional<uint64_t>, bool> ps_get(uint64_t cl) {
      if (auto* v = ps_chip.find(cl)) { ++dbg_ps_hits; return {*v, false}; }
      auto it = ps_dram.find(cl);
      if (it != ps_dram.end()) { ++dbg_ps_hits; ps_chip.insert(cl, it->second); return {it->second, true}; }
      ++dbg_ps_misses;
      return {std::nullopt, false};
  }

  std::pair<std::optional<uint64_t>, bool> sp_get(uint64_t sa) {
      if (auto* v = sp_chip.find(sa)) { ++dbg_sp_hits; return {*v, false}; }
      auto it = sp_dram.find(sa);
      if (it != sp_dram.end()) { ++dbg_sp_hits; sp_chip.insert(sa, it->second); return {it->second, true}; }
      ++dbg_sp_misses;
      return {std::nullopt, false};
  }

  template <typename Map>
  void evict_if_full(Map& m) {
      if (m.size() >= ISB_DRAM_MAX)
          m.erase(m.begin());
  }

  void ps_set(uint64_t cl, uint64_t sa) {
      evict_if_full(ps_dram);
      ps_chip.insert(cl, sa);
      ps_dram[cl] = sa;
      ps_conf[cl] = 1;
  }
  void sp_set(uint64_t sa, uint64_t cl) {
      evict_if_full(sp_dram);
      sp_chip.insert(sa, cl);
      sp_dram[sa] = cl;
  }

  uint8_t conf(uint64_t cl) {
      auto it = ps_conf.find(cl); return it != ps_conf.end() ? it->second : 0;
  }
  void inc_conf(uint64_t cl) { auto& c = ps_conf[cl]; if (c < ISB_CONF_MAX) { ++c; ++dbg_conf_inc; } }
  void dec_conf(uint64_t cl) { auto& c = ps_conf[cl]; if (c > 0) { --c; ++dbg_conf_dec; } }
};


class isb : public champsim::modules::prefetcher
{
  public:
    using prefetcher::prefetcher;

    void prefetcher_cycle_operate();

    uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip,
                                      uint8_t cache_hit, bool useful_prefetch,
                                      access_type type, uint32_t metadata_in);

    uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way,
                                  uint8_t prefetch, champsim::address evicted_addr,
                                  uint32_t metadata_in);

  private:
    ISBState state_{};
    void do_prefetch(uint64_t trigger_cl, uint64_t now);
};

#endif
