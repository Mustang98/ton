/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "ton/ton-tl.hpp"

#include "stats.h"

namespace ton::validator::consensus::stats {

std::unique_ptr<Id> Id::create(ShardIdFull shard, td::uint32 cc_seqno, size_t idx, size_t total_validators,
                               ValidatorWeight weight, ValidatorWeight total_weight,
                               td::uint32 slots_per_leader_window, td::uint32 target_rate_ms,
                               td::uint32 max_leader_window_desync) {
  return std::unique_ptr<Id>(new Id(shard.workchain, shard.shard, cc_seqno, idx, total_validators, weight, total_weight,
                                    slots_per_leader_window, target_rate_ms, max_leader_window_desync));
}

tl::EventRef Id::to_tl() const {
  return create_tl_object<tl::id>(workchain_, shard_, cc_seqno_, idx_, total_validators_, weight_, total_weight_,
                                  slots_per_leader_window_, target_rate_ms_, max_leader_window_desync_);
}

std::string Id::to_string() const {
  return PSTRING() << "Id{workchain=" << workchain_ << ", shard=" << shard_ << ", cc_seqno=" << cc_seqno_
                   << ", idx=" << idx_ << ", total_validators=" << total_validators_ << ", weight=" << weight_
                   << ", total_weight=" << total_weight_ << ", slots_per_leader_window=" << slots_per_leader_window_
                   << ", target_rate_ms=" << target_rate_ms_
                   << ", max_leader_window_desync=" << max_leader_window_desync_
                   << "}";
}

Id::Id(WorkchainId workchain, ShardId shard, td::uint32 cc_seqno, size_t idx, size_t total_validators,
       ValidatorWeight weight, ValidatorWeight total_weight, td::uint32 slots_per_leader_window,
       td::uint32 target_rate_ms, td::uint32 max_leader_window_desync)
    : workchain_(workchain)
    , shard_(shard)
    , cc_seqno_(cc_seqno)
    , idx_(idx)
    , total_validators_(total_validators)
    , weight_(weight)
    , total_weight_(total_weight)
    , slots_per_leader_window_(slots_per_leader_window)
    , target_rate_ms_(target_rate_ms)
    , max_leader_window_desync_(max_leader_window_desync) {
}

std::unique_ptr<CollateStarted> CollateStarted::create(td::uint32 slot, double slot_start) {
  return std::unique_ptr<CollateStarted>(new CollateStarted(slot, slot_start));
}

tl::EventRef CollateStarted::to_tl() const {
  return create_tl_object<tl::collateStarted>(target_slot_, slot_start_);
}

std::string CollateStarted::to_string() const {
  return PSTRING() << "CollateStarted{target_slot=" << target_slot_ << ", slot_start=" << slot_start_ << "}";
}

void CollateStarted::collect_to(MetricCollector& collector) const {
  collector.collect_collate_started(*this);
}

CollateStarted::CollateStarted(td::uint32 target_slot, double slot_start)
    : target_slot_(target_slot), slot_start_(slot_start) {
}

std::unique_ptr<CollateFinished> CollateFinished::create(td::uint32 slot, CandidateId id, double started_at,
                                                         double collated_at, double slot_start, double total_time,
                                                         double work_time) {
  return std::unique_ptr<CollateFinished>(
      new CollateFinished(slot, id, started_at, collated_at, slot_start, total_time, work_time));
}

tl::EventRef CollateFinished::to_tl() const {
  return create_tl_object<tl::collateFinished>(target_slot_, id_.to_tl(), started_at_, collated_at_, slot_start_,
                                               total_time_, work_time_);
}

std::string CollateFinished::to_string() const {
  return PSTRING() << "CollateFinished{target_slot=" << target_slot_ << ", id=" << id_
                   << ", started_at=" << started_at_ << ", collated_at=" << collated_at_
                   << ", slot_start=" << slot_start_ << ", total_time=" << total_time_
                   << ", work_time=" << work_time_ << "}";
}

void CollateFinished::collect_to(MetricCollector& collector) const {
  collector.collect_collate_finished(*this);
}

CollateFinished::CollateFinished(td::uint32 target_slot, CandidateId id, double started_at, double collated_at,
                                 double slot_start, double total_time, double work_time)
    : target_slot_(target_slot)
    , id_(id)
    , started_at_(started_at)
    , collated_at_(collated_at)
    , slot_start_(slot_start)
    , total_time_(total_time)
    , work_time_(work_time) {
}

std::unique_ptr<CollatedEmpty> CollatedEmpty::create(CandidateId id) {
  return std::unique_ptr<CollatedEmpty>(new CollatedEmpty(id));
}

tl::EventRef CollatedEmpty::to_tl() const {
  return create_tl_object<tl::collatedEmpty>(id_.to_tl());
}

std::string CollatedEmpty::to_string() const {
  return PSTRING() << "CollatedEmpty{id=" << id_ << "}";
}

CollatedEmpty::CollatedEmpty(CandidateId id) : id_(id) {
}

std::unique_ptr<CandidateBroadcastQueued> CandidateBroadcastQueued::create(const CandidateRef& candidate) {
  return std::unique_ptr<CandidateBroadcastQueued>(
      new CandidateBroadcastQueued(candidate->id, candidate->generated_at, candidate->slot_start));
}

tl::EventRef CandidateBroadcastQueued::to_tl() const {
  return create_tl_object<tl::candidateBroadcastQueued>(id_.to_tl(), generated_at_, slot_start_);
}

std::string CandidateBroadcastQueued::to_string() const {
  return PSTRING() << "CandidateBroadcastQueued{id=" << id_ << ", generated_at=" << generated_at_
                   << ", slot_start=" << slot_start_ << "}";
}

CandidateBroadcastQueued::CandidateBroadcastQueued(CandidateId id, double generated_at, double slot_start)
    : id_(id), generated_at_(generated_at), slot_start_(slot_start) {
}

std::unique_ptr<CandidateReceived> CandidateReceived::create(const CandidateRef& candidate, bool is_collator) {
  auto empty_fn = [&](const BlockIdExt&) { return std::optional<BlockIdExt>{}; };
  auto candidate_fn = [&](const BlockCandidate& candidate_block) { return std::optional{candidate_block.id}; };
  auto block = std::visit(td::overloaded(empty_fn, candidate_fn), candidate->block);

  return std::unique_ptr<CandidateReceived>(
      new CandidateReceived(candidate->id, candidate->parent_id, block, is_collator, candidate->generated_at,
                            candidate->slot_start, candidate->overlay_received_at));
}

tl::EventRef CandidateReceived::to_tl() const {
  tl::CandidateBlockRef block;
  if (block_.has_value()) {
    block = create_tl_object<tl::block>(create_tl_block_id(*block_));
  } else {
    block = create_tl_object<tl::empty>();
  }
  return create_tl_object<tl::candidateReceived>(id_.to_tl(), CandidateId::parent_id_to_tl(parent_), std::move(block),
                                                 is_collator_, generated_at_, slot_start_, overlay_received_at_);
}

std::string CandidateReceived::to_string() const {
  std::string block_str = "empty";
  if (block_.has_value()) {
    block_str = block_->to_str();
  }
  return PSTRING() << "CandidateReceived{id=" << id_ << ", parent=" << parent_ << ", block_id=" << block_str
                   << ", generated_at=" << generated_at_ << ", slot_start=" << slot_start_
                   << ", overlay_received_at=" << overlay_received_at_ << "}";
}

void CandidateReceived::collect_to(MetricCollector& collector) const {
  collector.collect_candidate_received(*this);
}

CandidateReceived::CandidateReceived(CandidateId id, ParentId parent, std::optional<BlockIdExt> block, bool is_collator,
                                     double generated_at, double slot_start, double overlay_received_at)
    : id_(id)
    , parent_(parent)
    , block_(block)
    , is_collator_(is_collator)
    , generated_at_(generated_at)
    , slot_start_(slot_start)
    , overlay_received_at_(overlay_received_at) {
}

std::unique_ptr<ValidationStarted> ValidationStarted::create(CandidateId id) {
  return std::unique_ptr<ValidationStarted>(new ValidationStarted(id));
}

tl::EventRef ValidationStarted::to_tl() const {
  return create_tl_object<tl::validationStarted>(id_.to_tl());
}

std::string ValidationStarted::to_string() const {
  return PSTRING() << "ValidationStarted{id=" << id_ << "}";
}

void ValidationStarted::collect_to(MetricCollector& collector) const {
  collector.collect_validation_started(*this);
}

ValidationStarted::ValidationStarted(CandidateId id) : id_(id) {
}

std::unique_ptr<ValidationFinished> ValidationFinished::create(CandidateId id) {
  return std::unique_ptr<ValidationFinished>(new ValidationFinished(id));
}

tl::EventRef ValidationFinished::to_tl() const {
  return create_tl_object<tl::validationFinished>(id_.to_tl());
}

std::string ValidationFinished::to_string() const {
  return PSTRING() << "ValidationFinished{id=" << id_ << "}";
}

void ValidationFinished::collect_to(MetricCollector& collector) const {
  collector.collect_validation_finished(*this);
}

ValidationFinished::ValidationFinished(CandidateId id) : id_(id) {
}

std::unique_ptr<ValidationReady> ValidationReady::create(CandidateId id, double ok_from_utime) {
  return std::unique_ptr<ValidationReady>(new ValidationReady(id, ok_from_utime));
}

tl::EventRef ValidationReady::to_tl() const {
  return create_tl_object<tl::validationReady>(id_.to_tl(), ok_from_utime_);
}

std::string ValidationReady::to_string() const {
  return PSTRING() << "ValidationReady{id=" << id_ << ", ok_from_utime=" << ok_from_utime_ << "}";
}

void ValidationReady::collect_to(MetricCollector& collector) const {
  collector.collect_validation_ready(*this);
}

ValidationReady::ValidationReady(CandidateId id, double ok_from_utime) : id_(id), ok_from_utime_(ok_from_utime) {
}

std::unique_ptr<BlockAccepted> BlockAccepted::create(CandidateId id, double accept_started_at) {
  return std::unique_ptr<BlockAccepted>(new BlockAccepted(id, accept_started_at));
}

tl::EventRef BlockAccepted::to_tl() const {
  return create_tl_object<tl::blockAccepted>(id_.to_tl(), accept_started_at_);
}

std::string BlockAccepted::to_string() const {
  return PSTRING() << "BlockAccepted{id=" << id_ << ", accept_started_at=" << accept_started_at_ << "}";
}

void BlockAccepted::collect_to(MetricCollector& collector) const {
  collector.collect_block_accepted(*this);
}

BlockAccepted::BlockAccepted(CandidateId id, double accept_started_at)
    : id_(id), accept_started_at_(accept_started_at) {
}

}  // namespace ton::validator::consensus::stats
