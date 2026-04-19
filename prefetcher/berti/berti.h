#ifndef PREFETCHER_BERTI_H
#define PREFETCHER_BERTI_H

#include <cstdint>
#include <array>

#include "champsim.h"
#include "modules.h"
#include "cache.h"
#include "msl/fwcounter.h"


constexpr std::size_t IP_TABLE_SIZE       = 64;   // direct-mapped IP entries
constexpr std::size_t HISTORY_SIZE        = 8;    // circular history depth per IP
constexpr std::size_t MAX_DELTAS          = 4;    // delta slots per IP entry
constexpr std::size_t CURRENT_PAGES_SIZE  = 16;   // hot-page table entries
constexpr std::size_t RECORDED_PAGES_SIZE = 16;   // cold-page table entries
constexpr int         BURST_MAX           = 3;    // burst prefetches per cold-page hit
constexpr std::size_t MISS_TABLE_SIZE     = 64;   // outstanding miss tracking entries

constexpr uint8_t COV_MAX       = 15;  // 4-bit saturating counter ceiling
constexpr uint8_t L1D_THRESHOLD = 12;  // coverage >= this → fill to L1D
constexpr uint8_t L2_THRESHOLD  = 6;   // coverage >= this → fill to L2

constexpr float MSHR_THRESHOLD = 0.70f; // demote to L2 when MSHR > 70% full

constexpr int BERTI_LOG2_BLOCK = 6;
constexpr int BERTI_LOG2_PAGE  = 12;
constexpr int PAGE_LINES = 1 << (BERTI_LOG2_PAGE - BERTI_LOG2_BLOCK); // 64 lines per page

constexpr int64_t DELTA_MAX = (1LL << 15);

static inline uint64_t cl_to_page(uint64_t cl_addr)
{
    return cl_addr >> (BERTI_LOG2_PAGE - BERTI_LOG2_BLOCK);  // i.e. >> 6
}

static inline int cl_to_offset(uint64_t cl_addr)
{
    return static_cast<int>(cl_addr & static_cast<uint64_t>(PAGE_LINES - 1));
}

enum class DeltaStatus : uint8_t {
  NO_PREF  = 0,  // too uncertain — suppress
  L2_PREF  = 1,  // medium confidence — fill to L2
  L1D_PREF = 2,  // high confidence   — fill to L1D
};

static inline DeltaStatus cov_to_status(uint8_t cov)
{
    if (cov >= L1D_THRESHOLD) return DeltaStatus::L1D_PREF;
    if (cov >= L2_THRESHOLD)  return DeltaStatus::L2_PREF;
    return DeltaStatus::NO_PREF;
}

struct HistEntry {
  uint64_t va_cl = 0;     // virtual cache-line address (byte >> LOG2_BLOCK)
  uint64_t cycle = 0;     // simulation cycle this demand was issued
  bool     valid = false;
};

struct DeltaEntry {
  int32_t     delta    = 0;
  uint8_t     coverage = 0;
  DeltaStatus status   = DeltaStatus::NO_PREF;

  void increment() {
      if (coverage < COV_MAX) ++coverage;
      status = cov_to_status(coverage);
  }
  void decrement() {
      if (coverage > 0) --coverage;
      status = cov_to_status(coverage);
  }
};

struct IPEntry {
  uint64_t tag      = 0;
  bool     valid    = false;

  std::array<HistEntry,  HISTORY_SIZE> hist{};
  int hist_head = 0;   // write pointer for the ring

  std::array<DeltaEntry, MAX_DELTAS> deltas{};
  int num_deltas = 0;

  // Push a new (virtual cl addr, cycle) pair into the history ring.
  void push(uint64_t va_cl, uint64_t cycle) {
      hist[hist_head] = {va_cl, cycle, true};
      hist_head = (hist_head + 1) % static_cast<int>(HISTORY_SIZE);
  }

  // Return a pointer to the DeltaEntry for delta d, allocating if needed.
  // Eviction policy: replace the lowest-coverage entry when full.
  DeltaEntry* get_or_alloc(int32_t d) {
      for (int i = 0; i < num_deltas; ++i)
          if (deltas[i].delta == d) return &deltas[i];

      if (num_deltas < static_cast<int>(MAX_DELTAS)) {
          deltas[num_deltas] = {d, 0, DeltaStatus::NO_PREF};
          return &deltas[num_deltas++];
      }
      // Evict minimum-coverage entry
      int min_i = 0;
      for (int i = 1; i < num_deltas; ++i)
          if (deltas[i].coverage < deltas[min_i].coverage) min_i = i;
      deltas[min_i] = {d, 0, DeltaStatus::NO_PREF};
      return &deltas[min_i];
  }
};

struct CurPageEntry {
  uint64_t page     = 0;
  uint64_t accessed = 0;  // bit[i] = 1 → offset i has been accessed
  bool     valid    = false;
};

struct RecPageEntry {
  uint64_t page       = 0;
  uint64_t accessed   = 0;   // snapshot bitmask
  int32_t  best_delta = 0;   // best delta at eviction time (informational)
  int      burst_ptr  = 0;   // resume point for burst prefetching
  bool     valid      = false;
};

// Tracks which IP caused each outstanding miss for correct fill-time training
struct MissEntry {
  uint64_t va_cl = 0;
  uint64_t ip    = 0;
  uint64_t cycle = 0;
  bool     valid = false;
};

struct BertiState {

  std::array<IPEntry,       IP_TABLE_SIZE>       ip_table{};
  std::array<CurPageEntry,  CURRENT_PAGES_SIZE>  cur_pages{};
  std::array<RecPageEntry,  RECORDED_PAGES_SIZE> rec_pages{};
  std::array<MissEntry,     MISS_TABLE_SIZE>      miss_table{};

  int         cur_lru   = 0;
  int         rec_lru   = 0;
  std::size_t miss_head = 0;

  IPEntry* get_ip(uint64_t ip) {
      auto idx = static_cast<int>(ip % IP_TABLE_SIZE);
      auto& e  = ip_table[idx];
      if (!e.valid || e.tag != ip) {
          e       = IPEntry{};
          e.tag   = ip;
          e.valid = true;
      }
      return &e;
  }

  CurPageEntry* find_cur(uint64_t pg) {
      for (auto& e : cur_pages)
          if (e.valid && e.page == pg) return &e;
      return nullptr;
  }

  // Allocate a hot-page slot; evicts round-robin victim to cold table.
  CurPageEntry* alloc_cur(uint64_t pg, int32_t best_delta) {
      for (auto& e : cur_pages) {   // prefer empty slot
          if (!e.valid) { e = {pg, 0, true}; return &e; }
      }
      auto& victim = cur_pages[cur_lru];
      record(victim.page, victim.accessed, best_delta);
      victim  = {pg, 0, true};
      cur_lru = (cur_lru + 1) % static_cast<int>(CURRENT_PAGES_SIZE);
      return &victim;
  }

  RecPageEntry* find_rec(uint64_t pg) {
      for (auto& e : rec_pages)
          if (e.valid && e.page == pg) return &e;
      return nullptr;
  }

  // Record or update a cold-page entry.
  void record(uint64_t pg, uint64_t accessed, int32_t best_delta) {
      for (auto& e : rec_pages) {
          if (e.valid && e.page == pg) {
              e.accessed   |= accessed;
              e.best_delta  = best_delta;
              e.burst_ptr   = 0;
              return;
          }
      }
      for (auto& e : rec_pages) {
          if (!e.valid) { e = {pg, accessed, best_delta, 0, true}; return; }
      }
      rec_pages[rec_lru] = {pg, accessed, best_delta, 0, true};
      rec_lru = (rec_lru + 1) % static_cast<int>(RECORDED_PAGES_SIZE);
  }

  int32_t best_delta(IPEntry* e) {
      int32_t bd = 0; uint8_t bc = 0;
      for (int i = 0; i < e->num_deltas; ++i) {
          auto& d = e->deltas[i];
          if (d.status != DeltaStatus::NO_PREF && d.coverage > bc) {
              bc = d.coverage; bd = d.delta;
          }
      }
      return bd;
  }

  // Record a miss so prefetcher_cache_fill can train the correct IP entry
  void record_miss(uint64_t va_cl, uint64_t ip, uint64_t cycle) {
      // Update in-place if already tracking this cache line
      for (auto& e : miss_table) {
          if (e.valid && e.va_cl == va_cl) {
              e.ip    = ip;
              e.cycle = cycle;
              return;
          }
      }
      miss_table[miss_head] = {va_cl, ip, cycle, true};
      miss_head = (miss_head + 1) % MISS_TABLE_SIZE;
  }

  MissEntry* find_miss(uint64_t va_cl) {
      for (auto& e : miss_table)
          if (e.valid && e.va_cl == va_cl) return &e;
      return nullptr;
  }
};


class berti : public champsim::modules::prefetcher
{
public:
    using champsim::modules::prefetcher::prefetcher;

    // Called once when the cache is initialised.
    void prefetcher_initialize();

    // Called on every L1D tag check (hit or miss) for demand accesses.
    uint32_t prefetcher_cache_operate(champsim::address addr,
                                      champsim::address ip,
                                      uint8_t  cache_hit,
                                      bool useful_prefetch,
                                      access_type type,
                                      uint32_t metadata_in);

    // Called when a miss is filled into the L1D.  This is the training hook.
    uint32_t prefetcher_cache_fill(champsim::address addr,
                                   long set, long way,
                                   uint8_t prefetch,
                                   champsim::address evicted_addr,
                                   uint32_t metadata_in);

private:
    BertiState state_{};
};

#endif
