#include "cache.h"
#include "berti.h"


void berti::prefetcher_initialize() {}


uint32_t berti::prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                      uint32_t metadata_in)
{
  if(type == access_type::PREFETCH){
    return metadata_in;
  }

  const uint64_t now    = intern_->current_time.time_since_epoch() / intern_->clock_period;
  const uint64_t va_cl  = addr.to<uint64_t>() >> BERTI_LOG2_BLOCK;
  const uint64_t ip_val = ip.to<uint64_t>();
  const uint64_t pg     = cl_to_page(va_cl);
  const int      off    = cl_to_offset(va_cl);

  //Keeps track of pages + the page bitvector
  IPEntry* ip_e = state_.get_ip(ip_val);
  CurPageEntry* cp = state_.find_cur(pg);
  if (!cp)
      cp = state_.alloc_cur(pg, state_.best_delta(ip_e));

  cp->accessed |= (1ULL << off);
  //Record accessed address and time
  ip_e->push(va_cl, now);

  // Record misses so prefetcher_cache_fill can train the correct IP entry
  if (!cache_hit)
      state_.record_miss(va_cl, ip_val, now);

  //Prefetching logic
  const auto   mshr_occ  = intern_->get_mshr_occupancy_ratio();
  const bool   fill_l1   = (mshr_occ < MSHR_THRESHOLD);

  //If MSHR is not full, prefetch the line
  for (int i = 0; i < ip_e->num_deltas; ++i) {
      const DeltaEntry& de = ip_e->deltas[i];
      if (de.status == DeltaStatus::NO_PREF) continue;

      const uint64_t pf_cl  = static_cast<uint64_t>(
          static_cast<int64_t>(va_cl) + static_cast<int64_t>(de.delta));

      // Suppress if target is on this same page and already accessed.
      if (cl_to_page(pf_cl) == pg &&
          ((cp->accessed >> cl_to_offset(pf_cl)) & 1ULL))
          continue;

      const bool to_l1 = fill_l1 && (de.status == DeltaStatus::L1D_PREF);
      intern_->prefetch_line(champsim::address{pf_cl << BERTI_LOG2_BLOCK}, to_l1, 0);
  }

  //Prefetch cold pages
  RecPageEntry* rp = state_.find_rec(pg);
  if (rp && rp->burst_ptr < PAGE_LINES) {
      int issued = 0;
      for (int o = rp->burst_ptr;
            o < PAGE_LINES && issued < BURST_MAX;
            ++o)
      {
          if (!((rp->accessed >> o) & 1ULL)) continue;  // not seen before
          if ( (cp->accessed  >> o) & 1ULL)  continue;  // already this visit

          const uint64_t pf_cl = (pg << (BERTI_LOG2_PAGE - BERTI_LOG2_BLOCK))
                                  | static_cast<uint64_t>(o);
          intern_->prefetch_line(champsim::address{pf_cl << BERTI_LOG2_BLOCK},
                                  fill_l1, 0);
          ++issued;
          rp->burst_ptr = o + 1;
      }
  }

  return metadata_in;
}

uint32_t berti::prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in)
{
  (void)set; (void)way; (void)prefetch; (void)evicted_addr;

  const uint64_t fill_cl = addr.to<uint64_t>() >> BERTI_LOG2_BLOCK;
  const uint64_t now     = intern_->current_time.time_since_epoch() / intern_->clock_period;

  // Only train the IP responsible for this miss
  MissEntry* me = state_.find_miss(fill_cl);
  if (!me) return metadata_in;

  IPEntry* ip_e          = state_.get_ip(me->ip);
  const uint64_t latency = now - me->cycle;
  me->valid = false;  // consume the entry

  // Walk forward through history entries that came after the miss was issued
  for (int h = 0; h < static_cast<int>(HISTORY_SIZE); ++h) {
      HistEntry& he = ip_e->hist[h];
      if (!he.valid || he.cycle <= me->cycle) continue;

      // Compute the delta from the just-filled line to this entry
      const int64_t raw = static_cast<int64_t>(he.va_cl)
                        - static_cast<int64_t>(fill_cl);
      if (raw == 0 || raw > DELTA_MAX || raw < -DELTA_MAX) continue;

      const bool timely = (he.cycle < me->cycle + latency);

      DeltaEntry* de = ip_e->get_or_alloc(static_cast<int32_t>(raw));
      if (timely) de->increment();
      else        de->decrement();
  }

  return metadata_in;
}
