/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "types.h"

namespace ton::validator::consensus::stats {

namespace tl {

using block = ton_api::consensus_stats_block;
using empty = ton_api::consensus_stats_empty;
using CandidateBlock = ton_api::consensus_stats_CandidateBlock;
using CandidateBlockRef = tl_object_ptr<CandidateBlock>;

using id = ton_api::consensus_stats_id;
using collateStarted = ton_api::consensus_stats_collateStarted;
using collateFinished = ton_api::consensus_stats_collateFinished;
using collatedEmpty = ton_api::consensus_stats_collatedEmpty;
using candidateBroadcastQueued = ton_api::consensus_stats_candidateBroadcastQueued;
using candidateReceived = ton_api::consensus_stats_candidateReceived;
using validationStarted = ton_api::consensus_stats_validationStarted;
using validationFinished = ton_api::consensus_stats_validationFinished;
using validationReady = ton_api::consensus_stats_validationReady;
using blockAccepted = ton_api::consensus_stats_blockAccepted;
using Event = ton_api::consensus_stats_Event;
using EventRef = tl_object_ptr<Event>;

using timestampedEvent = ton_api::consensus_stats_timestampedEvent;
using TimestampedEventRef = tl_object_ptr<timestampedEvent>;

using events = ton_api::consensus_stats_events;
using EventsRef = tl_object_ptr<events>;

}  // namespace tl

class MetricCollector;

class Id : public Event {
 public:
  static std::unique_ptr<Id> create(ShardIdFull shard, td::uint32 cc_seqno, size_t idx, size_t total_validators,
                                    ValidatorWeight weight, ValidatorWeight total_weight,
                                    td::uint32 slots_per_leader_window, td::uint32 target_rate_ms,
                                    td::uint32 max_leader_window_desync);

  tl::EventRef to_tl() const override;
  std::string to_string() const override;

 private:
  Id(WorkchainId workchain, ShardId shard, td::uint32 cc_seqno, size_t idx, size_t total_validators,
     ValidatorWeight weight, ValidatorWeight total_weight, td::uint32 slots_per_leader_window,
     td::uint32 target_rate_ms, td::uint32 max_leader_window_desync);

  WorkchainId workchain_;
  ShardId shard_;
  td::uint32 cc_seqno_;
  size_t idx_;
  size_t total_validators_;
  ValidatorWeight weight_;
  ValidatorWeight total_weight_;
  td::uint32 slots_per_leader_window_;
  td::uint32 target_rate_ms_;
  td::uint32 max_leader_window_desync_;
};

class CollateStarted : public CollectibleEvent<MetricCollector> {
 public:
  static std::unique_ptr<CollateStarted> create(td::uint32 slot, double slot_start);

  tl::EventRef to_tl() const override;
  std::string to_string() const override;
  void collect_to(MetricCollector& collector) const override;

  td::uint32 target_slot() const {
    return target_slot_;
  }
  double slot_start() const {
    return slot_start_;
  }

 private:
  CollateStarted(td::uint32 target_slot, double slot_start);

  td::uint32 target_slot_;
  double slot_start_;
};

class CollateFinished : public CollectibleEvent<MetricCollector> {
 public:
  static std::unique_ptr<CollateFinished> create(td::uint32 slot, CandidateId id, double started_at,
                                                 double collated_at, double slot_start, double total_time,
                                                 double work_time);

  tl::EventRef to_tl() const override;
  std::string to_string() const override;
  void collect_to(MetricCollector& collector) const override;

  td::uint32 target_slot() const {
    return target_slot_;
  }
  CandidateId id() const {
    return id_;
  }
  double started_at() const {
    return started_at_;
  }
  double collated_at() const {
    return collated_at_;
  }
  double slot_start() const {
    return slot_start_;
  }
  double total_time() const {
    return total_time_;
  }
  double work_time() const {
    return work_time_;
  }

 private:
  CollateFinished(td::uint32 target_slot, CandidateId id, double started_at, double collated_at, double slot_start,
                  double total_time, double work_time);

  td::uint32 target_slot_;
  CandidateId id_;
  double started_at_;
  double collated_at_;
  double slot_start_;
  double total_time_;
  double work_time_;
};

class CollatedEmpty : public Event {
 public:
  static std::unique_ptr<CollatedEmpty> create(CandidateId id);

  tl::EventRef to_tl() const override;
  std::string to_string() const override;

  CandidateId id() const {
    return id_;
  }

 private:
  CollatedEmpty(CandidateId id);

  CandidateId id_;
};

class CandidateBroadcastQueued : public Event {
 public:
  static std::unique_ptr<CandidateBroadcastQueued> create(const CandidateRef& candidate);

  tl::EventRef to_tl() const override;
  std::string to_string() const override;

  CandidateId id() const {
    return id_;
  }
  double generated_at() const {
    return generated_at_;
  }
  double slot_start() const {
    return slot_start_;
  }

 private:
  CandidateBroadcastQueued(CandidateId id, double generated_at, double slot_start);

  CandidateId id_;
  double generated_at_;
  double slot_start_;
};

class CandidateReceived : public CollectibleEvent<MetricCollector> {
 public:
  static std::unique_ptr<CandidateReceived> create(const CandidateRef& candidate, bool is_collator);

  tl::EventRef to_tl() const override;
  std::string to_string() const override;
  void collect_to(MetricCollector& collector) const override;

  CandidateId id() const {
    return id_;
  }
  ParentId parent() const {
    return parent_;
  }
  std::optional<BlockIdExt> block_id() const {
    return block_;
  }
  bool is_collator() const {
    return is_collator_;
  }
  double generated_at() const {
    return generated_at_;
  }
  double slot_start() const {
    return slot_start_;
  }
  double overlay_received_at() const {
    return overlay_received_at_;
  }

 private:
  CandidateReceived(CandidateId id, ParentId parent, std::optional<BlockIdExt> block, bool is_collator,
                    double generated_at, double slot_start, double overlay_received_at);

  CandidateId id_;
  ParentId parent_;
  std::optional<BlockIdExt> block_;
  bool is_collator_;
  double generated_at_;
  double slot_start_;
  double overlay_received_at_;
};

class ValidationStarted : public CollectibleEvent<MetricCollector> {
 public:
  static std::unique_ptr<ValidationStarted> create(CandidateId id);

  tl::EventRef to_tl() const override;
  std::string to_string() const override;
  void collect_to(MetricCollector& collector) const override;

  CandidateId id() const {
    return id_;
  }

 private:
  ValidationStarted(CandidateId id);

  CandidateId id_;
};

class ValidationFinished : public CollectibleEvent<MetricCollector> {
 public:
  static std::unique_ptr<ValidationFinished> create(CandidateId id);

  tl::EventRef to_tl() const override;
  std::string to_string() const override;
  void collect_to(MetricCollector& collector) const override;

  CandidateId id() const {
    return id_;
  }

 private:
  ValidationFinished(CandidateId id);

  CandidateId id_;
};

class ValidationReady : public CollectibleEvent<MetricCollector> {
 public:
  static std::unique_ptr<ValidationReady> create(CandidateId id, double ok_from_utime);

  tl::EventRef to_tl() const override;
  std::string to_string() const override;
  void collect_to(MetricCollector& collector) const override;

  CandidateId id() const {
    return id_;
  }
  double ok_from_utime() const {
    return ok_from_utime_;
  }

 private:
  ValidationReady(CandidateId id, double ok_from_utime);

  CandidateId id_;
  double ok_from_utime_;
};

class BlockAccepted : public CollectibleEvent<MetricCollector> {
 public:
  static std::unique_ptr<BlockAccepted> create(CandidateId id, double accept_started_at);

  tl::EventRef to_tl() const override;
  std::string to_string() const override;
  void collect_to(MetricCollector& collector) const override;

  CandidateId id() const {
    return id_;
  }
  double accept_started_at() const {
    return accept_started_at_;
  }

 private:
  BlockAccepted(CandidateId id, double accept_started_at);

  CandidateId id_;
  double accept_started_at_;
};

class MetricCollector {
 public:
  virtual ~MetricCollector() = default;

  virtual void collect_collate_started(const CollateStarted& event) = 0;
  virtual void collect_collate_finished(const CollateFinished& event) = 0;
  virtual void collect_candidate_received(const CandidateReceived& event) = 0;
  virtual void collect_validation_started(const ValidationStarted& event) = 0;
  virtual void collect_validation_finished(const ValidationFinished& event) = 0;
  virtual void collect_validation_ready(const ValidationReady& event) = 0;
  virtual void collect_block_accepted(const BlockAccepted& event) = 0;
};

}  // namespace ton::validator::consensus::stats
