#ifndef PREFETCHER_TRIANGEL_H
#define PREFETCHER_TRIANGEL_H

#include <cstdint>
#include <array>
#include <unordered_map>
#include <vector>

#include "champsim.h"
#include "modules.h"
#include "cache.h"


constexpr std::size_t TU_SIZE            = 512;
constexpr std::size_t MARKOV_MAX         = 8192;

constexpr uint8_t     CONF_MAX           = 15;
constexpr uint8_t     CONF_INIT          = 4;    // high_pat threshold for lookahead
constexpr uint8_t     CONF_STORE_MIN     = 1;    // minimum confidence for SCS/HS training

constexpr uint8_t     BASEPAT_DEC        = 1;
constexpr uint8_t     HIGHPAT_DEC        = 2;

constexpr int         DEGREE_HIGH        = 4;
constexpr int         DEGREE_LOW         = 1;

constexpr std::size_t HS_SIZE            = 512;
constexpr std::size_t HS_WAYS            = 4;   // bumped for fewer set conflicts
constexpr std::size_t SCS_SIZE           = 64;
constexpr uint64_t    SCS_WINDOW         = 512;

constexpr std::size_t MRB_SIZE           = 256;
constexpr std::size_t MRB_WAYS           = 2;

constexpr uint64_t    DUELLER_WINDOW     = 500000;
constexpr std::size_t DUELLER_SETS       = 64;

constexpr std::size_t DATA_WAYS_MAX      = 32;
constexpr std::size_t MARKOV_WAYS_MAX    = 16;

constexpr uint32_t    MARKOV_HIT_WEIGHT  = 6;

constexpr std::size_t MARKOV_MIN         = 2048;

struct TUEntry {
  uint64_t ip         = 0;
  uint64_t cl1        = 0;
  uint64_t cl2        = 0;
  uint32_t local_ts   = 0;

  uint8_t  base_pat   = 0;
  uint8_t  high_pat   = 0;
  uint8_t  reuse_conf = 0;
  uint8_t  sample_rate= 12;  // sample every access

  bool     lookahead  = false;
  bool     valid      = false;
};

struct MarkovEntry {
  uint64_t trigger    = 0;
  uint64_t target     = 0;
  bool     conf_bit   = false;
};

struct HSEntry {
  uint64_t trigger    = 0;
  uint64_t target     = 0;
  uint16_t tu_idx     = 0;
  uint32_t local_ts   = 0;
  bool     valid      = false;
};

struct SCSEntry {
  uint64_t trigger    = 0;
  uint64_t target     = 0;
  uint16_t tu_idx     = 0;
  uint64_t ready_by   = 0;
  bool     valid      = false;
};

struct MRBEntry {
  uint64_t trigger    = 0;
  uint64_t target     = 0;
  bool     valid      = false;
};

struct TriangelState {

  std::array<TUEntry, TU_SIZE> tu{};

  std::unordered_map<uint64_t, MarkovEntry> markov{};
  std::vector<uint64_t> markov_fifo{};   // tracks insertion order for FIFO eviction
  std::size_t markov_cap = MARKOV_MAX;

  std::array<HSEntry, HS_SIZE> hs{};
  std::array<uint8_t, HS_SIZE / HS_WAYS> hs_fifo{};

  std::array<SCSEntry, SCS_SIZE> scs{};
  std::size_t scs_head = 0;

  std::array<MRBEntry, MRB_SIZE> mrb{};
  std::array<uint8_t, MRB_SIZE / MRB_WAYS> mrb_fifo{};

  uint64_t dueller_accesses = 0;
  std::array<uint32_t, MARKOV_WAYS_MAX + 1> dueller_scores{};
  std::array<uint16_t, DUELLER_SETS> sampled_sets{};

  struct DuellerSet {
    std::array<uint16_t, DATA_WAYS_MAX>    data_tags{};
    std::array<bool,     DATA_WAYS_MAX>    data_valid{};
    std::array<uint16_t, MARKOV_WAYS_MAX>  markov_tags{};
    std::array<bool,     MARKOV_WAYS_MAX>  markov_valid{};
  };
  std::array<DuellerSet, DUELLER_SETS> dueller_sets{};

  uint32_t llc_sets    = 2048;
  uint32_t data_ways   = 16;
  uint32_t markov_ways = 8;
  uint32_t markov_step = 1;

  static std::size_t tu_index(uint64_t ip) { return ip % TU_SIZE; }

  static uint16_t hash10(uint64_t x) { return static_cast<uint16_t>((x ^ (x >> 11) ^ (x >> 21)) & 0x3FFu); }

  std::size_t sampled_set_index(uint16_t llc_set) const {
    for (std::size_t i = 0; i < DUELLER_SETS; ++i)
      if (sampled_sets[i] == llc_set) return i;
    return DUELLER_SETS;
  }

  template <std::size_t W>
  static std::size_t lru_access(std::array<uint16_t, W>& tags, std::array<bool, W>& valid, uint16_t tag, std::size_t active_ways)
  {
    if (active_ways == 0) return 0;
    std::size_t hit_pos = 0;
    const std::size_t lim = std::min<std::size_t>(W, active_ways);
    for (std::size_t i = 0; i < lim; ++i) {
      if (valid[i] && tags[i] == tag) { hit_pos = i + 1; break; }
    }

    if (hit_pos != 0) {
      const uint16_t t = tags[hit_pos - 1];
      for (std::size_t i = hit_pos - 1; i > 0; --i) {
        tags[i]  = tags[i - 1];
        valid[i] = valid[i - 1];
      }
      tags[0]  = t;
      valid[0] = true;
      return hit_pos;
    }

    for (std::size_t i = lim - 1; i > 0; --i) {
      tags[i]  = tags[i - 1];
      valid[i] = valid[i - 1];
    }
    tags[0]  = tag;
    valid[0] = true;
    return 0;
  }

  TUEntry* get_tu(uint64_t ip) {
      auto& e = tu[tu_index(ip)];
      if (!e.valid || e.ip != ip) {
        e             = TUEntry{};
        e.ip          = ip;
        e.valid       = true;
        e.base_pat    = CONF_STORE_MIN + 1;
        e.high_pat    = 0;
        e.reuse_conf  = CONF_STORE_MIN;
        e.sample_rate = 12;
      }
      return &e;
  }

  MarkovEntry* markov_lookup(uint64_t trigger) {
      auto it = markov.find(trigger);
      return (it != markov.end()) ? &it->second : nullptr;
  }

  void markov_store(uint64_t trigger, uint64_t target) {
      auto it = markov.find(trigger);
      if (it != markov.end()) {
          if (it->second.target == target) {
              it->second.conf_bit = true;
          } else if (!it->second.conf_bit) {
              it->second.target   = target;
              it->second.conf_bit = false;
          } else {
              it->second.conf_bit = false;
          }
          return;
      }
      // Use markov_fifo for deterministic FIFO eviction
      if (markov.size() >= markov_cap) {
          if (!markov_fifo.empty()) {
              markov.erase(markov_fifo.front());
              markov_fifo.erase(markov_fifo.begin());
          }
      }
      markov.emplace(trigger, MarkovEntry{trigger, target, false});
      markov_fifo.push_back(trigger);
  }

  uint64_t* mrb_lookup(uint64_t trigger) {
      const std::size_t sets = MRB_SIZE / MRB_WAYS;
      const std::size_t set  = trigger % sets;
      const std::size_t base = set * MRB_WAYS;
      for (std::size_t w = 0; w < MRB_WAYS; ++w) {
          auto& e = mrb[base + w];
          if (e.valid && e.trigger == trigger) return &e.target;
      }
      return nullptr;
  }

  void mrb_insert(uint64_t trigger, uint64_t target) {
      const std::size_t sets = MRB_SIZE / MRB_WAYS;
      const std::size_t set  = trigger % sets;
      const std::size_t base = set * MRB_WAYS;

      for (std::size_t w = 0; w < MRB_WAYS; ++w) {
          auto& e = mrb[base + w];
          if (e.valid && e.trigger == trigger) { e.target = target; return; }
      }

      const std::size_t way = mrb_fifo[set] % MRB_WAYS;
      mrb[base + way] = {trigger, target, true};
      mrb_fifo[set] = static_cast<uint8_t>((way + 1) % MRB_WAYS);
  }

  HSEntry* hs_find(uint64_t trigger, uint16_t tu_idx) {
      const std::size_t sets = HS_SIZE / HS_WAYS;
      const std::size_t set  = trigger % sets;
      const std::size_t base = set * HS_WAYS;
      for (std::size_t w = 0; w < HS_WAYS; ++w) {
        auto& e = hs[base + w];
        if (e.valid && e.trigger == trigger && e.tu_idx == tu_idx) return &e;
      }
      return nullptr;
  }

  void hs_insert(uint64_t trigger, uint64_t target, uint16_t tu_idx, uint32_t local_ts) {
      const std::size_t sets = HS_SIZE / HS_WAYS;
      const std::size_t set  = trigger % sets;
      const std::size_t base = set * HS_WAYS;

      for (std::size_t w = 0; w < HS_WAYS; ++w) {
        auto& e = hs[base + w];
        if (e.valid && e.trigger == trigger && e.tu_idx == tu_idx) {
          e.target   = target;
          e.local_ts = local_ts;
          return;
        }
      }

      const std::size_t way = hs_fifo[set] % HS_WAYS;
      hs[base + way] = {trigger, target, tu_idx, local_ts, true};
      hs_fifo[set] = static_cast<uint8_t>((way + 1) % HS_WAYS);
  }

  SCSEntry* scs_find(uint64_t target, uint16_t tu_idx) {
      for (auto& e : scs)
          if (e.valid && e.target == target && e.tu_idx == tu_idx) return &e;
      return nullptr;
  }

  void scs_insert(uint64_t trigger, uint64_t target, uint16_t tu_idx, uint64_t cycle) {
      if (scs[scs_head].valid) {
        auto& te = tu[scs[scs_head].tu_idx % TU_SIZE];
        if (te.valid) {
          te.base_pat = (te.base_pat > BASEPAT_DEC) ? static_cast<uint8_t>(te.base_pat - BASEPAT_DEC) : 0;
          te.high_pat = (te.high_pat > HIGHPAT_DEC) ? static_cast<uint8_t>(te.high_pat - HIGHPAT_DEC) : 0;
        }
      }

      scs[scs_head] = {trigger, target, tu_idx, cycle + SCS_WINDOW, true};
      scs_head = (scs_head + 1) % SCS_SIZE;
  }
};



class triangel : public champsim::modules::prefetcher
{
  public:
    using prefetcher::prefetcher;

    void prefetcher_initialize();
    void prefetcher_cycle_operate();

    uint32_t prefetcher_cache_operate(champsim::address addr,
                                      champsim::address ip,
                                      uint8_t cache_hit,
                                      bool useful_prefetch,
                                      access_type type,
                                      uint32_t metadata_in);

    uint32_t prefetcher_cache_fill(champsim::address addr,
                                  long set, long way,
                                  uint8_t prefetch,
                                  champsim::address evicted_addr,
                                  uint32_t metadata_in);

    void prefetcher_final_stats();

  private:
    TriangelState state_{};

    uint64_t now() const;

    void get_degree(const TUEntry* te, int& degree) const {
      if (te->base_pat < CONF_STORE_MIN) {
          degree = 0;
      } else if (te->high_pat >= CONF_INIT) {
          degree = DEGREE_HIGH;
      } else {
          degree = DEGREE_LOW;
      }
    }

    // Issues prefetches directly; pending queue removed since
    // prefetcher_cycle_operate is not guaranteed to be called
    void issue_prefetches(uint64_t trigger, int degree);

    void dueller_init();
    void dueller_on_data_access(uint64_t cl);
    void dueller_on_markov_access(uint64_t trigger_cl);
    void dueller_maybe_resize();
};

#endif
