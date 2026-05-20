/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2017-2020 Telegram Systems LLP
*/
#include "common/delay.h"
#include "adnl/adnl-address-list.h"
#include "crypto/block/block.h"
#include "impl/out-msg-queue-proof.hpp"
#include "interfaces/validator-full-id.h"
#include "td/actor/MultiPromise.h"
#include "td/actor/coro_utils.h"
#include "td/utils/filesystem.h"
#include "td/utils/JsonBuilder.h"
#include "td/utils/overloaded.h"
#include "td/utils/port/IPAddress.h"
#include "td/utils/port/path.h"
#include "td/utils/port/Stat.h"
#include "td/utils/Random.h"
#include "ton/ton-io.hpp"
#include "ton/ton-tl.hpp"

#include "full-node.h"
#include "full-node.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace ton {

namespace validator {

namespace fullnode {

static const double INACTIVE_SHARD_TTL = (double)overlay::Overlays::overlay_peer_ttl() + 60.0;
static const td::uint32 OVERLAY_OBSERVER_MAX_ACTIVE_ADDRESS_RESOLVES = 4;
static const td::uint32 OVERLAY_OBSERVER_NO_ADDRESS_PROBE_QUERIES = 1;
static const double OVERLAY_OBSERVER_NO_ADDRESS_PROBE_MIN_INTERVAL = 30.0;
static const size_t OVERLAY_OBSERVER_RECENT_NEIGHBOURS_LIMIT = 100;
static const td::uint64 OVERLAY_OBSERVER_MIN_PEER_SHRINK_TO_REFUSE = 100;
static const td::uint64 OVERLAY_OBSERVER_FEC_PARTS_ROTATE_BYTES = 10ULL * 1024 * 1024 * 1024;
static const td::uint64 OVERLAY_OBSERVER_FEC_PARTS_RETAIN_BYTES = 100ULL * 1024 * 1024 * 1024;

std::string observer_json_escape(td::Slice s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) >= 0x80) {
          char buf[7];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

std::string observer_json_quote(td::Slice s) {
  return "\"" + observer_json_escape(s) + "\"";
}

void observer_write_json_string_array(std::ostream &os, const std::vector<std::string> &values) {
  os << "[";
  bool first = true;
  for (const auto &value : values) {
    if (!first) {
      os << ",";
    }
    first = false;
    os << observer_json_quote(value);
  }
  os << "]";
}

std::string observer_adnl_id_hex(const adnl::AdnlNodeIdShort &id) {
  return id.bits256_value().to_hex();
}

std::string observer_public_key_hash_hex(const PublicKeyHash &id) {
  return id.bits256_value().to_hex();
}

std::string observer_overlay_id_hex(const overlay::OverlayIdShort &id) {
  return id.bits256_value().to_hex();
}

std::string observer_overlay_target_key(const overlay::OverlayIdShort &overlay_id, const adnl::AdnlNodeIdShort &peer) {
  return observer_overlay_id_hex(overlay_id) + ":" + observer_adnl_id_hex(peer);
}

bool observer_is_hex256(td::Slice value) {
  if (value.size() != 64) {
    return false;
  }
  for (char c : value) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
      return false;
    }
  }
  return true;
}

std::string observer_bits256_hex(const td::Bits256 &bits) {
  if (bits.is_zero()) {
    return {};
  }
  auto hex = bits.to_hex();
  return observer_is_hex256(hex) ? hex : std::string();
}

std::string observer_fec_type_name(td::int32 type) {
  switch (type) {
    case ton_api::overlay_broadcastFec::ID:
      return "overlay.broadcastFec";
    case ton_api::overlay_broadcastFecShort::ID:
      return "overlay.broadcastFecShort";
    case ton_api::overlay_broadcastTwostepFec::ID:
      return "overlay.broadcastTwostepFec";
    default:
      return PSTRING() << "overlay.unknownFec(" << type << ")";
  }
}

td::Result<adnl::AdnlNodeIdShort> observer_parse_adnl_hex(td::Slice value) {
  td::Bits256 bits;
  if (!block::parse_hex_hash(value, bits)) {
    return td::Status::Error("bad ADNL hex");
  }
  return adnl::AdnlNodeIdShort{bits};
}

td::Result<overlay::OverlayIdShort> observer_parse_overlay_hex(td::Slice value) {
  td::Bits256 bits;
  if (!block::parse_hex_hash(value, bits)) {
    return td::Status::Error("bad overlay hex");
  }
  return overlay::OverlayIdShort{bits};
}

td::Status observer_mkpath_parent(td::Slice path) {
  auto s = path.str();
  auto pos = s.find_last_of(TD_DIR_SLASH);
  if (pos == std::string::npos) {
    return td::Status::OK();
  }
  return td::mkpath(s.substr(0, pos));
}

bool observer_update_recent_neighbours(std::vector<std::string> &recent_neighbours, td::Slice peer_hex) {
  if (!observer_is_hex256(peer_hex)) {
    return false;
  }
  auto value = peer_hex.str();
  auto it = std::find(recent_neighbours.begin(), recent_neighbours.end(), value);
  if (it != recent_neighbours.end() && it == recent_neighbours.begin()) {
    return false;
  }
  if (it != recent_neighbours.end()) {
    recent_neighbours.erase(it);
  }
  recent_neighbours.insert(recent_neighbours.begin(), std::move(value));
  while (recent_neighbours.size() > OVERLAY_OBSERVER_RECENT_NEIGHBOURS_LIMIT) {
    recent_neighbours.pop_back();
  }
  return true;
}

std::string observer_timestamp_utc() {
  std::time_t now = std::time(nullptr);
  std::tm tm{};
#if TD_PORT_POSIX
  gmtime_r(&now, &tm);
#else
  auto *value = std::gmtime(&now);
  if (value != nullptr) {
    tm = *value;
  }
#endif
  char buf[32];
  if (std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &tm) == 0) {
    return PSTRING() << static_cast<td::int64>(now);
  }
  return buf;
}

bool observer_has_prefix(td::Slice value, td::Slice prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool observer_has_suffix(td::Slice value, td::Slice suffix) {
  return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
}

void FullNodeImpl::add_permanent_key(PublicKeyHash key, td::Promise<td::Unit> promise) {
  if (local_keys_.count(key)) {
    promise.set_value(td::Unit());
    return;
  }

  local_keys_.insert(key);
  for (auto &p : custom_overlays_) {
    update_custom_overlay(p.second);
  }

  if (!sign_cert_by_.is_zero()) {
    promise.set_value(td::Unit());
    return;
  }

  for (auto &x : all_validators_) {
    if (x == key) {
      sign_cert_by_ = key;
    }
  }

  for (auto &shard : shards_) {
    if (!shard.second.actor.empty()) {
      td::actor::send_closure(shard.second.actor, &FullNodeShard::update_validators, all_validators_, sign_cert_by_);
    }
  }
  promise.set_value(td::Unit());
}

void FullNodeImpl::del_permanent_key(PublicKeyHash key, td::Promise<td::Unit> promise) {
  if (!local_keys_.count(key)) {
    promise.set_value(td::Unit());
    return;
  }
  local_keys_.erase(key);
  update_validator_telemetry_collector();
  for (auto &p : custom_overlays_) {
    update_custom_overlay(p.second);
  }

  if (sign_cert_by_ != key) {
    promise.set_value(td::Unit());
    return;
  }
  sign_cert_by_ = PublicKeyHash::zero();

  for (auto &x : all_validators_) {
    if (local_keys_.count(x)) {
      sign_cert_by_ = x;
    }
  }

  for (auto &shard : shards_) {
    if (!shard.second.actor.empty()) {
      td::actor::send_closure(shard.second.actor, &FullNodeShard::update_validators, all_validators_, sign_cert_by_);
    }
  }
  promise.set_value(td::Unit());
}

void FullNodeImpl::add_collator_adnl_id(adnl::AdnlNodeIdShort id) {
  ++local_collator_nodes_[id];
}

void FullNodeImpl::del_collator_adnl_id(adnl::AdnlNodeIdShort id) {
  if (--local_collator_nodes_[id] == 0) {
    local_collator_nodes_.erase(id);
  }
}

void FullNodeImpl::sign_shard_overlay_certificate(ShardIdFull shard_id, PublicKeyHash signed_key, td::uint32 expiry_at,
                                                  td::uint32 max_size, td::Promise<td::BufferSlice> promise) {
  auto it = shards_.find(shard_id);
  if (it == shards_.end() || it->second.actor.empty()) {
    promise.set_error(td::Status::Error(ErrorCode::error, "shard not found"));
    return;
  }
  td::actor::send_closure(it->second.actor, &FullNodeShard::sign_overlay_certificate, signed_key, expiry_at, max_size,
                          std::move(promise));
}

void FullNodeImpl::import_shard_overlay_certificate(ShardIdFull shard_id, PublicKeyHash signed_key,
                                                    std::shared_ptr<ton::overlay::Certificate> cert,
                                                    td::Promise<td::Unit> promise) {
  auto it = shards_.find(shard_id);
  if (it == shards_.end() || it->second.actor.empty()) {
    promise.set_error(td::Status::Error(ErrorCode::error, "shard not found"));
    return;
  }
  td::actor::send_closure(it->second.actor, &FullNodeShard::import_overlay_certificate, signed_key, cert,
                          std::move(promise));
}

void FullNodeImpl::update_adnl_id(adnl::AdnlNodeIdShort adnl_id, td::Promise<td::Unit> promise) {
  adnl_id_ = adnl_id;

  td::MultiPromise mp;
  auto ig = mp.init_guard();
  ig.add_promise(std::move(promise));

  for (auto &s : shards_) {
    if (!s.second.actor.empty()) {
      td::actor::send_closure(s.second.actor, &FullNodeShard::update_adnl_id, adnl_id, ig.get_promise());
    }
  }

  for (auto &p : custom_overlays_) {
    update_custom_overlay(p.second);
  }
}

void FullNodeImpl::set_config(FullNodeConfig config) {
  opts_.config_ = config;
  for (auto &s : shards_) {
    if (!s.second.actor.empty()) {
      td::actor::send_closure(s.second.actor, &FullNodeShard::set_config, config);
    }
  }
  for (auto &overlay : custom_overlays_) {
    for (auto &actor : overlay.second.actors_) {
      td::actor::send_closure(actor.second, &FullNodeCustomOverlay::set_config, config);
    }
  }
}

void FullNodeImpl::add_custom_overlay(CustomOverlayParams params, td::Promise<td::Unit> promise) {
  if (params.nodes_.empty()) {
    promise.set_error(td::Status::Error("list of nodes is empty"));
    return;
  }
  std::string name = params.name_;
  if (custom_overlays_.count(name)) {
    promise.set_error(td::Status::Error(PSTRING() << "duplicate custom overlay name \"" << name << "\""));
    return;
  }
  VLOG(FULL_NODE_WARNING) << "Adding custom overlay \"" << name << "\", " << params.nodes_.size() << " nodes";
  auto &p = custom_overlays_[name];
  p.params_ = std::move(params);
  update_custom_overlay(p);
  promise.set_result(td::Unit());
}

void FullNodeImpl::del_custom_overlay(std::string name, td::Promise<td::Unit> promise) {
  auto it = custom_overlays_.find(name);
  if (it == custom_overlays_.end()) {
    promise.set_error(td::Status::Error(PSTRING() << "no such overlay \"" << name << "\""));
    return;
  }
  custom_overlays_.erase(it);
  promise.set_result(td::Unit());
}

void FullNodeImpl::initial_read_complete(BlockHandle top_handle) {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    R.ensure();
    td::actor::send_closure(SelfId, &FullNodeImpl::sync_completed);
  });
  auto it = shards_.find(ShardIdFull{masterchainId});
  CHECK(it != shards_.end() && !it->second.actor.empty());
  td::actor::send_closure(it->second.actor, &FullNodeShard::set_handle, top_handle, std::move(P));
}

void FullNodeImpl::on_new_masterchain_block(td::Ref<MasterchainState> state, std::set<ShardIdFull> shards_to_monitor) {
  CHECK(shards_to_monitor.count(ShardIdFull(masterchainId)));
  bool join_all_overlays = !sign_cert_by_.is_zero();
  std::set<ShardIdFull> all_shards;
  std::set<ShardIdFull> new_active;
  all_shards.insert(ShardIdFull(masterchainId));
  std::set<WorkchainId> workchains;
  wc_monitor_min_split_ = state->monitor_min_split_depth(basechainId);
  auto cut_shard = [&](ShardIdFull shard) -> ShardIdFull {
    return wc_monitor_min_split_ < shard.pfx_len() ? shard_prefix(shard, wc_monitor_min_split_) : shard;
  };
  for (auto &info : state->get_shards()) {
    workchains.insert(info->shard().workchain);
    ShardIdFull shard = cut_shard(info->shard());
    while (true) {
      all_shards.insert(shard);
      if (shard.pfx_len() == 0) {
        break;
      }
      shard = shard_parent(shard);
    }
  }
  for (const auto &[wc, winfo] : state->get_workchain_list()) {
    if (!workchains.contains(wc) && winfo->active && winfo->enabled_since <= state->get_unix_time()) {
      all_shards.insert(ShardIdFull(wc));
    }
  }
  for (ShardIdFull shard : shards_to_monitor) {
    shard = cut_shard(shard);
    while (true) {
      new_active.insert(shard);
      if (shard.pfx_len() == 0) {
        break;
      }
      shard = shard_parent(shard);
    }
  }

  for (auto it = shards_.begin(); it != shards_.end();) {
    if (all_shards.contains(it->first)) {
      ++it;
    } else {
      it = shards_.erase(it);
    }
  }
  for (ShardIdFull shard : all_shards) {
    bool active = new_active.contains(shard);
    bool overlay_exists = !shards_[shard].actor.empty();
    if (active || join_all_overlays || overlay_exists) {
      update_shard_actor(shard, active);
    }
  }

  for (auto &[_, shard_info] : shards_) {
    if (!shard_info.active && shard_info.delete_at && shard_info.delete_at.is_in_past() && !join_all_overlays) {
      shard_info.actor = {};
      shard_info.overlay_observer_seeded = false;
      shard_info.delete_at = td::Timestamp::never();
    }
  }

  std::set<adnl::AdnlNodeIdShort> my_adnl_ids;
  my_adnl_ids.insert(adnl_id_);
  for (const auto &[adnl_id, _] : local_collator_nodes_) {
    my_adnl_ids.insert(adnl_id);
  }
  for (auto key : local_keys_) {
    auto it = current_validators_.find(key);
    if (it != current_validators_.end()) {
      my_adnl_ids.insert(it->second);
    }
  }
  std::set<ShardIdFull> monitoring_shards;
  for (ShardIdFull shard : shards_to_monitor) {
    monitoring_shards.insert(cut_shard(shard));
  }
  fast_sync_overlays_.update_overlays(state, std::move(my_adnl_ids), std::move(monitoring_shards),
                                      zero_state_file_hash_, opts_.fast_sync_broadcast_speed_multiplier_, keyring_,
                                      adnl_, rldp2_, quic_, overlays_, validator_manager_, actor_id(this));
  update_validator_telemetry_collector();
}

void FullNodeImpl::update_shard_actor(ShardIdFull shard, bool active) {
  ShardInfo &info = shards_[shard];
  if (info.actor.empty()) {
    info.actor =
        FullNodeShard::create(shard, local_id_, adnl_id_, zero_state_file_hash_, opts_, limiter_, keyring_, adnl_,
                              rldp_, rldp2_, overlays_, validator_manager_, client_, actor_id(this), active);
    if (!all_validators_.empty()) {
      td::actor::send_closure(info.actor, &FullNodeShard::update_validators, all_validators_, sign_cert_by_);
    }
  } else if (info.active != active) {
    td::actor::send_closure(info.actor, &FullNodeShard::set_active, active);
  }
  info.active = active;
  info.delete_at = active ? td::Timestamp::never() : td::Timestamp::in(INACTIVE_SHARD_TTL);
  seed_overlay_observer_peers(shard, info);
}

void FullNodeImpl::sync_completed() {
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::sync_complete, [](td::Result<>) {});
}

void FullNodeImpl::send_ihr_message(AccountIdPrefixFull dst, td::BufferSlice data) {
  auto shard = get_shard(dst);
  if (shard.empty()) {
    VLOG(FULL_NODE_WARNING) << "dropping OUT ihr message to unknown shard";
    return;
  }
  td::actor::send_closure(shard, &FullNodeShard::send_ihr_message, std::move(data));
}

void FullNodeImpl::send_ext_message(AccountIdPrefixFull dst, td::BufferSlice data) {
  bool skip_public = false;
  for (auto &[_, private_overlay] : custom_overlays_) {
    if (private_overlay.params_.send_shard(dst.as_leaf_shard())) {
      for (auto &[local_id, actor] : private_overlay.actors_) {
        if (private_overlay.params_.msg_senders_.contains(local_id)) {
          td::actor::send_closure(actor, &FullNodeCustomOverlay::send_external_message, data.clone());
          if (private_overlay.params_.skip_public_msg_send_) {
            skip_public = true;
          }
        }
      }
    }
  }

  if (!skip_public) {
    auto shard = get_shard(dst);
    if (shard.empty()) {
      VLOG(FULL_NODE_WARNING) << "dropping OUT ext message to unknown shard";
      return;
    }
    td::actor::send_closure(shard, &FullNodeShard::send_external_message, std::move(data));
  }
}

void FullNodeImpl::send_shard_block_info(BlockIdExt block_id, CatchainSeqno cc_seqno, td::BufferSlice data) {
  send_shard_block_info_to_custom_overlays(block_id, cc_seqno, data);
  auto shard = get_shard(ShardIdFull{masterchainId});
  if (shard.empty()) {
    VLOG(FULL_NODE_WARNING) << "dropping OUT shard block info message to unknown shard";
    return;
  }
  auto fast_sync_overlay = fast_sync_overlays_.choose_overlay(ShardIdFull(masterchainId)).first;
  if (!fast_sync_overlay.empty()) {
    td::actor::send_closure(fast_sync_overlay, &FullNodeFastSyncOverlay::send_shard_block_info, block_id, cc_seqno,
                            data.clone());
  }
  td::actor::send_closure(shard, &FullNodeShard::send_shard_block_info, block_id, cc_seqno, std::move(data));
}

void FullNodeImpl::send_block_candidate(BlockIdExt block_id, CatchainSeqno cc_seqno, td::uint32 validator_set_hash,
                                        td::BufferSlice data, int mode) {
  if (mode & broadcast_mode_custom) {
    send_block_candidate_broadcast_to_custom_overlays(block_id, cc_seqno, validator_set_hash, data);
  }
  if (mode & broadcast_mode_fast_sync) {
    auto fast_sync_overlay = fast_sync_overlays_.choose_overlay(block_id.shard_full()).first;
    if (!fast_sync_overlay.empty()) {
      td::actor::send_closure(fast_sync_overlay, &FullNodeFastSyncOverlay::send_block_candidate, block_id, cc_seqno,
                              validator_set_hash, data.clone());
    }
  }
  if (mode & broadcast_mode_public) {
    auto shard = get_shard(ShardIdFull{masterchainId, shardIdAll});
    if (shard.empty()) {
      VLOG(FULL_NODE_WARNING) << "dropping OUT shard block info message to unknown shard";
      return;
    }
    td::actor::send_closure(shard, &FullNodeShard::send_block_candidate, block_id, cc_seqno, validator_set_hash,
                            std::move(data));
  }
}

void FullNodeImpl::send_out_msg_queue_proof_broadcast(td::Ref<OutMsgQueueProofBroadcast> broadcast) {
  auto fast_sync_overlay = fast_sync_overlays_.choose_overlay(broadcast->dst_shard).first;
  if (!fast_sync_overlay.empty()) {
    td::actor::send_closure(fast_sync_overlay, &FullNodeFastSyncOverlay::send_out_msg_queue_proof_broadcast,
                            std::move(broadcast));
  }
}

void FullNodeImpl::send_broadcast(BlockBroadcast broadcast, int mode) {
  if (mode & broadcast_mode_custom) {
    send_block_broadcast_to_custom_overlays(broadcast);
  }
  if (mode & broadcast_mode_fast_sync) {
    auto fast_sync_overlay = fast_sync_overlays_.choose_overlay(broadcast.block_id.shard_full()).first;
    if (!fast_sync_overlay.empty()) {
      td::actor::send_closure(fast_sync_overlay, &FullNodeFastSyncOverlay::send_broadcast, broadcast.clone());
    }
  }
  if (mode & broadcast_mode_public) {
    auto shard = get_shard(broadcast.block_id.shard_full());
    if (shard.empty()) {
      VLOG(FULL_NODE_WARNING) << "dropping OUT broadcast to unknown shard";
      return;
    }
    td::actor::send_closure(shard, &FullNodeShard::send_broadcast, std::move(broadcast));
  }
}

void FullNodeImpl::download_block(BlockIdExt id, td::uint32 priority, td::Timestamp timeout,
                                  td::Promise<ReceivedBlock> promise) {
  auto shard = get_shard(id.shard_full());
  if (shard.empty()) {
    VLOG(FULL_NODE_WARNING) << "dropping download block query to unknown shard";
    promise.set_error(td::Status::Error(ErrorCode::notready, "shard not ready"));
    return;
  }
  td::actor::send_closure(shard, &FullNodeShard::download_block, id, priority, timeout, std::move(promise));
}

void FullNodeImpl::download_zero_state(BlockIdExt id, td::uint32 priority, td::Timestamp timeout,
                                       td::Promise<td::BufferSlice> promise) {
  auto shard = get_shard(id.shard_full());
  if (shard.empty()) {
    VLOG(FULL_NODE_WARNING) << "dropping download state query to unknown shard";
    promise.set_error(td::Status::Error(ErrorCode::notready, "shard not ready"));
    return;
  }
  td::actor::send_closure(shard, &FullNodeShard::download_zero_state, id, priority, timeout, std::move(promise));
}

void FullNodeImpl::download_persistent_state(BlockIdExt id, BlockIdExt masterchain_block_id, PersistentStateType type,
                                             td::uint32 priority, td::Timestamp timeout,
                                             td::Promise<td::BufferSlice> promise) {
  auto shard = get_shard(id.shard_full(), /* historical = */ true);
  if (shard.empty()) {
    VLOG(FULL_NODE_WARNING) << "dropping download state diff query to unknown shard";
    promise.set_error(td::Status::Error(ErrorCode::notready, "shard not ready"));
    return;
  }
  td::actor::send_closure(shard, &FullNodeShard::download_persistent_state, id, masterchain_block_id, type, priority,
                          timeout, std::move(promise));
}

void FullNodeImpl::download_block_proof(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                                        td::Promise<td::BufferSlice> promise) {
  auto shard = get_shard(block_id.shard_full());
  if (shard.empty()) {
    VLOG(FULL_NODE_WARNING) << "dropping download proof query to unknown shard";
    promise.set_error(td::Status::Error(ErrorCode::notready, "shard not ready"));
    return;
  }
  td::actor::send_closure(shard, &FullNodeShard::download_block_proof, block_id, priority, timeout, std::move(promise));
}

void FullNodeImpl::download_block_proof_link(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                                             td::Promise<td::BufferSlice> promise) {
  auto shard = get_shard(block_id.shard_full(), /* historical = */ true);
  if (shard.empty()) {
    VLOG(FULL_NODE_WARNING) << "dropping download proof link query to unknown shard";
    promise.set_error(td::Status::Error(ErrorCode::notready, "shard not ready"));
    return;
  }
  td::actor::send_closure(shard, &FullNodeShard::download_block_proof_link, block_id, priority, timeout,
                          std::move(promise));
}

void FullNodeImpl::get_next_key_blocks(BlockIdExt block_id, td::Timestamp timeout,
                                       td::Promise<std::vector<BlockIdExt>> promise) {
  auto shard = get_shard(block_id.shard_full());
  if (shard.empty()) {
    VLOG(FULL_NODE_WARNING) << "dropping download proof link query to unknown shard";
    promise.set_error(td::Status::Error(ErrorCode::notready, "shard not ready"));
    return;
  }
  td::actor::send_closure(shard, &FullNodeShard::get_next_key_blocks, block_id, timeout, std::move(promise));
}

void FullNodeImpl::download_archive(BlockSeqno masterchain_seqno, ShardIdFull shard_prefix, std::string tmp_dir,
                                    td::Timestamp timeout, td::Promise<std::string> promise) {
  auto shard = get_shard(shard_prefix, /* historical = */ true);
  if (shard.empty()) {
    VLOG(FULL_NODE_WARNING) << "dropping download archive query to unknown shard";
    promise.set_error(td::Status::Error(ErrorCode::notready, "shard not ready"));
    return;
  }
  CHECK(!shard.empty());
  td::actor::send_closure(shard, &FullNodeShard::download_archive, masterchain_seqno, shard_prefix, std::move(tmp_dir),
                          timeout, std::move(promise));
}

void FullNodeImpl::download_out_msg_queue_proof(ShardIdFull dst_shard, std::vector<BlockIdExt> blocks,
                                                block::ImportedMsgQueueLimits limits, td::Timestamp timeout,
                                                td::Promise<std::vector<td::Ref<OutMsgQueueProof>>> promise) {
  if (blocks.empty()) {
    promise.set_value({});
    return;
  }
  // All blocks are expected to have the same minsplit shard prefix
  auto shard = get_shard(blocks[0].shard_full());
  if (shard.empty()) {
    VLOG(FULL_NODE_WARNING) << "dropping download msg queue query to unknown shard";
    promise.set_error(td::Status::Error(ErrorCode::notready, "shard not ready"));
    return;
  }
  td::actor::send_closure(shard, &FullNodeShard::download_out_msg_queue_proof, dst_shard, std::move(blocks), limits,
                          timeout, std::move(promise));
}

td::actor::ActorId<FullNodeShard> FullNodeImpl::get_shard(ShardIdFull shard, bool historical) {
  if (shard.is_masterchain()) {
    return shards_[ShardIdFull{masterchainId}].actor.get();
  }
  if (shard.workchain != basechainId) {
    return {};
  }
  int pfx_len = shard.pfx_len();
  int min_split = wc_monitor_min_split_;
  if (historical) {
    min_split = td::Random::fast(0, min_split);
  }
  if (pfx_len > min_split) {
    shard = shard_prefix(shard, min_split);
  }
  while (true) {
    auto it = shards_.find(shard);
    if (it != shards_.end()) {
      update_shard_actor(shard, it->second.active);
      return it->second.actor.get();
    }
    if (shard.pfx_len() == 0) {
      break;
    }
    shard = shard_parent(shard);
  }

  // Special case if shards_ was not yet initialized.
  // This can happen briefly on node startup.
  return shards_[ShardIdFull{masterchainId}].actor.get();
}

td::actor::ActorId<FullNodeShard> FullNodeImpl::get_shard(AccountIdPrefixFull dst) {
  return get_shard(shard_prefix(dst, max_shard_pfx_len));
}

void FullNodeImpl::got_key_block_config(td::Ref<ConfigHolder> config) {
  PublicKeyHash l = PublicKeyHash::zero();
  std::vector<PublicKeyHash> keys;
  std::map<PublicKeyHash, adnl::AdnlNodeIdShort> current_validators;
  for (td::int32 i = -1; i <= 1; i++) {
    auto r = config->get_total_validator_set(i < 0 ? i : 1 - i);
    if (r.not_null()) {
      auto vec = r->export_vector();
      for (auto &el : vec) {
        auto key = ValidatorFullId{el.key}.compute_short_id();
        keys.push_back(key);
        if (local_keys_.count(key)) {
          l = key;
        }
        if (i == 1) {
          current_validators[key] = adnl::AdnlNodeIdShort{el.addr.is_zero() ? key.bits256_value() : el.addr};
        }
      }
    }
  }

  if (current_validators != current_validators_) {
    current_validators_ = std::move(current_validators);
    update_private_overlays();
  }

  // Let's turn off this optimization, since keyblocks are rare enough to update on each keyblock
  // if (keys == all_validators_) {
  //   return;
  // }

  all_validators_ = keys;
  sign_cert_by_ = l;
  CHECK(all_validators_.size() > 0);

  for (auto &shard : shards_) {
    if (!shard.second.actor.empty()) {
      td::actor::send_closure(shard.second.actor, &FullNodeShard::update_validators, all_validators_, sign_cert_by_);
    }
  }
}

void FullNodeImpl::new_key_block(BlockHandle handle) {
  if (handle->id().seqno() == 0) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
      if (R.is_error()) {
        VLOG(FULL_NODE_WARNING) << "failed to get zero state: " << R.move_as_error();
      } else {
        auto s = td::Ref<MasterchainState>{R.move_as_ok()};
        CHECK(s.not_null());
        td::actor::send_closure(SelfId, &FullNodeImpl::got_key_block_config, s->get_config_holder().move_as_ok());
      }
    });
    td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_shard_state_from_db, handle,
                            std::move(P));
  } else {
    CHECK(handle->is_key_block());
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ProofLink>> R) {
      if (R.is_error()) {
        VLOG(FULL_NODE_WARNING) << "failed to get key block proof: " << R.move_as_error();
      } else {
        td::actor::send_closure(SelfId, &FullNodeImpl::got_key_block_config,
                                R.ok()->get_key_block_config().move_as_ok());
      }
    });
    td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_block_proof_link_from_db, handle,
                            std::move(P));
  }
}

void FullNodeImpl::process_block_broadcast(BlockBroadcast broadcast, bool signatures_checked) {
  send_block_broadcast_to_custom_overlays(broadcast);
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::new_block_broadcast, std::move(broadcast),
                          signatures_checked, [](td::Result<td::Unit> R) {
                            if (R.is_error()) {
                              if (R.error().code() == ErrorCode::notready) {
                                LOG(DEBUG) << "dropped broadcast: " << R.move_as_error();
                              } else {
                                LOG(INFO) << "dropped broadcast: " << R.move_as_error();
                              }
                            }
                          });
}

void FullNodeImpl::process_block_candidate_broadcast(BlockIdExt block_id, CatchainSeqno cc_seqno,
                                                     td::uint32 validator_set_hash, td::BufferSlice data) {
  send_block_candidate_broadcast_to_custom_overlays(block_id, cc_seqno, validator_set_hash, data);
  td::actor::ask(validator_manager_, &ValidatorManagerInterface::new_block_candidate_broadcast, block_id, cc_seqno,
                 std::move(data))
      .detach();
}

void FullNodeImpl::process_shard_block_info_broadcast(BlockIdExt block_id, CatchainSeqno cc_seqno,
                                                      td::BufferSlice data) {
  send_shard_block_info_to_custom_overlays(block_id, cc_seqno, data);
  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::new_shard_block_description_broadcast,
                          block_id, cc_seqno, std::move(data));
}

void FullNodeImpl::get_out_msg_queue_query_token(td::Promise<std::unique_ptr<ActionToken>> promise) {
  td::actor::send_closure(out_msg_queue_query_token_manager_, &TokenManager::get_token, 1, 0, td::Timestamp::in(10.0),
                          std::move(promise));
}

void FullNodeImpl::set_validator_telemetry_filename(std::string value) {
  validator_telemetry_filename_ = std::move(value);
  update_validator_telemetry_collector();
}

void FullNodeImpl::update_validator_telemetry_collector() {
  if (validator_telemetry_filename_.empty()) {
    validator_telemetry_collector_key_ = PublicKeyHash::zero();
    return;
  }
  if (fast_sync_overlays_.get_masterchain_overlay_for(adnl::AdnlNodeIdShort{validator_telemetry_collector_key_})
          .empty()) {
    auto [actor, adnl_id] = fast_sync_overlays_.choose_overlay(ShardIdFull{masterchainId});
    validator_telemetry_collector_key_ = adnl_id.pubkey_hash();
    if (!actor.empty()) {
      td::actor::send_closure(actor, &FullNodeFastSyncOverlay::collect_validator_telemetry,
                              validator_telemetry_filename_);
    }
  }
}

void FullNodeImpl::update_dht_node(td::actor::ActorId<dht::Dht> dht) {
  dht_ = dht;
  pump_overlay_observer_address_resolves();
}

void FullNodeImpl::init_overlay_observer() {
  if (!opts_.overlay_observer_.enabled_ || overlay_observer_initialized_) {
    return;
  }
  overlay_observer_initialized_ = true;
  overlay_observer_dir_ = opts_.overlay_observer_.output_dir_.empty()
                              ? PSTRING() << db_root_ << "/full-node-overlay-observer"
                              : opts_.overlay_observer_.output_dir_;
  auto S = td::mkpath(overlay_observer_dir_);
  if (S.is_error()) {
    LOG(WARNING) << "failed to create full-node overlay observer dir " << overlay_observer_dir_ << ": " << S;
    return;
  }
  overlay_observer_peers_path_ = overlay_observer_dir_ + "/overlay-peers.json";
  overlay_observer_fec_parts_path_ = overlay_observer_dir_ + "/fec-parts.jsonl";
  overlay_observer_members_log_path_ = overlay_observer_dir_ + "/overlay-members.jsonl";
  load_overlay_observer_peer_state();
  load_overlay_observer_peers();

  open_overlay_observer_fec_parts_log();
  rotate_overlay_observer_fec_parts_if_needed();
  prune_overlay_observer_fec_parts_logs();
  overlay_observer_members_log_stream_.open(overlay_observer_members_log_path_, std::ios::out | std::ios::app);
  if (!overlay_observer_members_log_stream_) {
    LOG(WARNING) << "failed to open overlay members log " << overlay_observer_members_log_path_;
  }

  pump_overlay_observer_address_resolves();
  rebuild_overlay_observer_queue();
  overlay_observer_flush_at_ = td::Timestamp::in(30.0);
  alarm_timestamp().relax(td::Timestamp::now());
}

void FullNodeImpl::load_overlay_observer_peers() {
  if (opts_.overlay_observer_.peers_file_.empty()) {
    return;
  }
  auto data = td::read_file(opts_.overlay_observer_.peers_file_);
  if (data.is_error()) {
    LOG(WARNING) << "failed to read full-node overlay observer peers " << opts_.overlay_observer_.peers_file_ << ": "
                 << data.move_as_error();
    return;
  }
  auto data_buf = data.move_as_ok();
  auto json = td::json_decode(data_buf.as_slice());
  if (json.is_error()) {
    LOG(WARNING) << "failed to parse full-node overlay observer peers " << opts_.overlay_observer_.peers_file_ << ": "
                 << json.move_as_error();
    return;
  }
  td::uint64 loaded = 0;
  auto load_peer = [&](const td::JsonValue &item) {
    if (item.type() != td::JsonValue::Type::Object) {
      return;
    }
    const auto &obj = item.get_object();
    auto node_hex = obj.get_optional_string_field("node");
    if (node_hex.is_error()) {
      node_hex = obj.get_optional_string_field("adnl_id");
    }
    auto overlay_hex = obj.get_optional_string_field("overlay");
    if (overlay_hex.is_error()) {
      overlay_hex = obj.get_optional_string_field("last_overlay");
    }
    if (node_hex.is_error() || overlay_hex.is_error()) {
      return;
    }
    auto node = observer_parse_adnl_hex(node_hex.move_as_ok());
    auto overlay_id = observer_parse_overlay_hex(overlay_hex.move_as_ok());
    if (node.is_error() || overlay_id.is_error()) {
      return;
    }
    td::int32 version = 0;
    auto version_value = obj.get_optional_int_field("version", 0);
    if (version_value.is_ok()) {
      version = version_value.move_as_ok();
    }
    td::uint32 flags = 0;
    auto flags_value = obj.get_optional_int_field("flags", 0);
    if (flags_value.is_ok()) {
      flags = static_cast<td::uint32>(std::max<td::int32>(0, flags_value.move_as_ok()));
    }
    auto id = node.move_as_ok();
    auto oid = overlay_id.move_as_ok();
    if (add_overlay_observer_peer(oid, id, false, true, version, flags)) {
      loaded++;
    }
  };

  auto root = json.move_as_ok();
  if (root.type() == td::JsonValue::Type::Array) {
    for (const auto &item : root.get_array()) {
      load_peer(item);
    }
  } else if (root.type() == td::JsonValue::Type::Object) {
    root.get_object().foreach([&](td::Slice field_name, const td::JsonValue &field_value) {
      if (field_name == "peers" && field_value.type() == td::JsonValue::Type::Array) {
        for (const auto &item : field_value.get_array()) {
          load_peer(item);
        }
      }
    });
  } else {
    LOG(WARNING) << "full-node overlay observer peers root is neither object nor array: "
                 << opts_.overlay_observer_.peers_file_;
    return;
  }
  td::uint64 total = 0;
  for (const auto &[_, peers] : overlay_observer_initial_peers_) {
    total += peers.size();
  }
  LOG(WARNING) << "full-node overlay observer loaded " << loaded << " new peers and " << total << " total peers in "
               << overlay_observer_initial_peers_.size() << " overlays from " << opts_.overlay_observer_.peers_file_;
}

bool FullNodeImpl::load_overlay_observer_peer_state() {
  auto data = td::read_file(overlay_observer_peers_path_);
  if (data.is_error()) {
    return false;
  }
  auto data_buf = data.move_as_ok();
  auto json = td::json_decode(data_buf.as_slice());
  if (json.is_error()) {
    LOG(WARNING) << "failed to parse " << overlay_observer_peers_path_ << ": " << json.move_as_error();
    return false;
  }

  auto load_fec = [&](const auto &obj, td::Slice peer_hex) {
    OverlayObserverFecSender stat;
    stat.adnl_id = peer_hex.str();
    auto first_seen = obj.get_optional_double_field("first_seen", 0.0);
    if (first_seen.is_ok()) {
      stat.first_seen = first_seen.move_as_ok();
    }
    auto last_seen = obj.get_optional_double_field("last_seen", 0.0);
    if (last_seen.is_ok()) {
      stat.last_seen = last_seen.move_as_ok();
    }
    auto fec_parts = obj.get_optional_long_field("fec_parts", 0);
    if (fec_parts.is_ok()) {
      stat.fec_parts = static_cast<td::uint64>(std::max<td::int64>(0, fec_parts.move_as_ok()));
    }
    auto last_type = obj.get_optional_string_field("last_type");
    if (last_type.is_ok()) {
      stat.last_type = last_type.move_as_ok();
    }
    auto load_hex256_field = [&](td::Slice field, std::string &out) {
      auto value = obj.get_optional_string_field(field);
      if (value.is_error()) {
        return;
      }
      auto str = value.move_as_ok();
      if (observer_is_hex256(str)) {
        out = std::move(str);
      }
    };
    load_hex256_field("last_origin_broadcaster_adnl_id", stat.last_origin_broadcaster_adnl_id);
    load_hex256_field("last_source_key_id", stat.last_source_key_id);
    load_hex256_field("last_data_hash", stat.last_data_hash);
    load_hex256_field("last_part_hash", stat.last_part_hash);
    load_hex256_field("last_part_data_hash", stat.last_part_data_hash);
    load_hex256_field("last_broadcast_hash", stat.last_broadcast_hash);
    auto last_seqno = obj.get_optional_int_field("last_seqno");
    if (last_seqno.is_ok()) {
      stat.last_seqno = last_seqno.move_as_ok();
      stat.has_last_seqno = true;
    }
    obj.foreach([&](td::Slice field_name, const td::JsonValue &field_value) {
      if (field_name != "parts_by_type" || field_value.type() != td::JsonValue::Type::Object) {
        return;
      }
      field_value.get_object().foreach([&](td::Slice type_name, const td::JsonValue &type_value) {
        if (type_value.type() != td::JsonValue::Type::Number) {
          return;
        }
        auto count = td::to_integer_safe<td::uint64>(type_value.get_number());
        if (count.is_ok()) {
          stat.parts_by_type[type_name.str()] = count.move_as_ok();
        }
      });
    });
    if (stat.first_seen != 0.0 || stat.last_seen != 0.0 || stat.fec_parts != 0 || !stat.last_type.empty() ||
        !stat.parts_by_type.empty()) {
      overlay_observer_fec_senders_[stat.adnl_id] = std::move(stat);
    }
  };

  auto load_peer = [&](const td::JsonValue &item) {
    if (item.type() != td::JsonValue::Type::Object) {
      return;
    }
    OverlayObserverMember member;
    const auto &obj = item.get_object();
    auto id = obj.get_optional_string_field("adnl_id");
    if (id.is_error()) {
      return;
    }
    member.adnl_id = id.move_as_ok();
    if (member.adnl_id.empty()) {
      return;
    }
    auto version = obj.get_optional_int_field("version", 0);
    if (version.is_ok()) {
      member.version = version.move_as_ok();
    }
    auto flags = obj.get_optional_int_field("flags", 0);
    if (flags.is_ok()) {
      member.flags = static_cast<td::uint32>(std::max<td::int32>(0, flags.move_as_ok()));
    }
    auto first_request_at = obj.get_optional_double_field("first_request_at", 0.0);
    if (first_request_at.is_ok()) {
      member.first_request_at = first_request_at.move_as_ok();
    }
    auto last_request_at = obj.get_optional_double_field("last_request_at", 0.0);
    if (last_request_at.is_ok()) {
      member.last_request_at = last_request_at.move_as_ok();
    }
    auto last_response_at = obj.get_optional_double_field("last_response_at", 0.0);
    if (last_response_at.is_ok()) {
      member.last_response_at = last_response_at.move_as_ok();
    }
    auto last_success_at = obj.get_optional_double_field("last_success_at", 0.0);
    if (last_success_at.is_ok()) {
      member.last_success_at = last_success_at.move_as_ok();
    }
    auto last_fail_at = obj.get_optional_double_field("last_fail_at", 0.0);
    if (last_fail_at.is_ok()) {
      member.last_fail_at = last_fail_at.move_as_ok();
    }
    auto last_latency = obj.get_optional_double_field("last_latency", 0.0);
    if (last_latency.is_ok()) {
      member.last_latency = last_latency.move_as_ok();
    }
    auto requests = obj.get_optional_long_field("requests", 0);
    if (requests.is_ok()) {
      member.requests = static_cast<td::uint64>(std::max<td::int64>(0, requests.move_as_ok()));
    }
    auto successes = obj.get_optional_long_field("successes", 0);
    if (successes.is_ok()) {
      member.successes = static_cast<td::uint64>(std::max<td::int64>(0, successes.move_as_ok()));
    }
    auto failures = obj.get_optional_long_field("failures", 0);
    if (failures.is_ok()) {
      member.failures = static_cast<td::uint64>(std::max<td::int64>(0, failures.move_as_ok()));
    }
    auto consecutive_failures = obj.get_optional_int_field("consecutive_failures", 0);
    if (consecutive_failures.is_ok()) {
      member.consecutive_failures = static_cast<td::uint32>(std::max<td::int32>(0, consecutive_failures.move_as_ok()));
    }
    auto last_returned_peers = obj.get_optional_int_field("last_returned_peers", 0);
    if (last_returned_peers.is_ok()) {
      member.last_returned_peers = static_cast<td::uint32>(std::max<td::int32>(0, last_returned_peers.move_as_ok()));
    }
    auto max_returned_peers = obj.get_optional_int_field("max_returned_peers", static_cast<td::int32>(member.last_returned_peers));
    if (max_returned_peers.is_ok()) {
      member.max_returned_peers = static_cast<td::uint32>(std::max<td::int32>(0, max_returned_peers.move_as_ok()));
    }
    if (member.max_returned_peers < member.last_returned_peers) {
      member.max_returned_peers = member.last_returned_peers;
    }
    auto last_overlay = obj.get_optional_string_field("last_overlay");
    if (last_overlay.is_ok()) {
      member.last_overlay = last_overlay.move_as_ok();
    }
    auto last_shard = obj.get_optional_string_field("last_shard");
    if (last_shard.is_ok()) {
      member.last_shard = last_shard.move_as_ok();
    }
    auto last_error = obj.get_optional_string_field("last_error");
    if (last_error.is_ok()) {
      member.last_error = last_error.move_as_ok();
    }
    auto address_updated_at = obj.get_optional_double_field("address_updated_at", 0.0);
    if (address_updated_at.is_ok()) {
      member.address_updated_at = address_updated_at.move_as_ok();
    }
    auto address_source = obj.get_optional_string_field("address_source");
    if (address_source.is_ok()) {
      member.address_source = address_source.move_as_ok();
    }
    auto last_dht_success_at = obj.get_optional_double_field("last_dht_success_at", 0.0);
    if (last_dht_success_at.is_ok()) {
      member.last_dht_success_at = last_dht_success_at.move_as_ok();
    }
    auto last_dht_fail_at = obj.get_optional_double_field("last_dht_fail_at", 0.0);
    if (last_dht_fail_at.is_ok()) {
      member.last_dht_fail_at = last_dht_fail_at.move_as_ok();
    }
    auto dht_successes = obj.get_optional_long_field("dht_successes", 0);
    if (dht_successes.is_ok()) {
      member.dht_successes = static_cast<td::uint64>(std::max<td::int64>(0, dht_successes.move_as_ok()));
    }
    auto dht_failures = obj.get_optional_long_field("dht_failures", 0);
    if (dht_failures.is_ok()) {
      member.dht_failures = static_cast<td::uint64>(std::max<td::int64>(0, dht_failures.move_as_ok()));
    }
    auto last_dht_error = obj.get_optional_string_field("last_dht_error");
    if (last_dht_error.is_ok()) {
      member.last_dht_error = last_dht_error.move_as_ok();
    }
    auto last_conn_ip = obj.get_optional_string_field("last_conn_ip");
    if (last_conn_ip.is_ok()) {
      member.last_conn_ip = last_conn_ip.move_as_ok();
    }
    auto last_conn_ip_at = obj.get_optional_double_field("last_conn_ip_at", 0.0);
    if (last_conn_ip_at.is_ok()) {
      member.last_conn_ip_at = last_conn_ip_at.move_as_ok();
    }
    auto success_query_but_no_ip_at = obj.get_optional_double_field("success_query_but_no_ip_at", 0.0);
    if (success_query_but_no_ip_at.is_ok()) {
      member.success_query_but_no_ip_at = success_query_but_no_ip_at.move_as_ok();
    }
    obj.foreach([&](td::Slice field_name, const td::JsonValue &field_value) {
      if (field_name == "neighbours" && field_value.type() == td::JsonValue::Type::Array) {
        for (const auto &neighbour : field_value.get_array()) {
          if (neighbour.type() == td::JsonValue::Type::String) {
            auto value = neighbour.get_string().str();
            if (!value.empty()) {
              member.neighbours.insert(std::move(value));
            }
          }
        }
        return;
      }
      if (field_name == "recent_neighbours" && field_value.type() == td::JsonValue::Type::Array) {
        for (const auto &neighbour : field_value.get_array()) {
          if (neighbour.type() != td::JsonValue::Type::String) {
            continue;
          }
          auto value = neighbour.get_string().str();
          if (observer_is_hex256(value) &&
              value != member.adnl_id &&
              std::find(member.recent_neighbours.begin(), member.recent_neighbours.end(), value) ==
                  member.recent_neighbours.end()) {
            member.recent_neighbours.push_back(std::move(value));
            if (member.recent_neighbours.size() >= OVERLAY_OBSERVER_RECENT_NEIGHBOURS_LIMIT) {
              break;
            }
          }
        }
        return;
      }
      if (field_name == "addresses" && field_value.type() == td::JsonValue::Type::Array) {
        for (const auto &address_value : field_value.get_array()) {
          if (address_value.type() != td::JsonValue::Type::Object) {
            continue;
          }
          const auto &address_obj = address_value.get_object();
          auto type = address_obj.get_optional_string_field("type");
          auto ip = address_obj.get_optional_string_field("ip");
          auto port = address_obj.get_optional_int_field("port", 0);
          if (type.is_error() || ip.is_error() || port.is_error()) {
            continue;
          }
          OverlayObserverAddress address;
          address.type = type.move_as_ok();
          address.ip = ip.move_as_ok();
          address.port = port.move_as_ok();
          if (!address.type.empty() && !address.ip.empty() && address.port > 0) {
            member.addresses.push_back(std::move(address));
          }
        }
        return;
      }
      if (field_name == "overlays" && field_value.type() == td::JsonValue::Type::Array) {
        for (const auto &overlay_value : field_value.get_array()) {
          if (overlay_value.type() == td::JsonValue::Type::String) {
            auto overlay_hex = overlay_value.get_string().str();
            if (observer_parse_overlay_hex(overlay_hex).is_ok()) {
              member.overlays.emplace(std::move(overlay_hex), std::string());
            }
            continue;
          }
          if (overlay_value.type() != td::JsonValue::Type::Object) {
            continue;
          }
          const auto &overlay_obj = overlay_value.get_object();
          auto overlay_hex = overlay_obj.get_optional_string_field("overlay");
          if (overlay_hex.is_error()) {
            continue;
          }
          auto overlay_str = overlay_hex.move_as_ok();
          if (observer_parse_overlay_hex(overlay_str).is_error()) {
            continue;
          }
          std::string shard_str;
          auto shard = overlay_obj.get_optional_string_field("shard");
          if (shard.is_ok()) {
            shard_str = shard.move_as_ok();
          }
          member.overlays[std::move(overlay_str)] = std::move(shard_str);
        }
        return;
      }
      if (field_name == "fec" && field_value.type() == td::JsonValue::Type::Object) {
        load_fec(field_value.get_object(), member.adnl_id);
      }
    });

    auto peer_hex = member.adnl_id;
    if (!member.last_overlay.empty() && observer_parse_overlay_hex(member.last_overlay).is_ok()) {
      member.overlays.emplace(member.last_overlay, member.last_shard);
    }
    std::vector<std::string> overlay_hexes;
    overlay_hexes.reserve(member.overlays.size());
    for (const auto &[overlay_hex, _] : member.overlays) {
      overlay_hexes.push_back(overlay_hex);
    }
    overlay_observer_members_[peer_hex] = std::move(member);
    auto parsed_peer = observer_parse_adnl_hex(peer_hex);
    if (parsed_peer.is_ok()) {
      auto peer = parsed_peer.move_as_ok();
      for (const auto &overlay_hex : overlay_hexes) {
        auto parsed_overlay = observer_parse_overlay_hex(overlay_hex);
        if (parsed_overlay.is_ok()) {
          add_overlay_observer_peer(parsed_overlay.move_as_ok(), peer, false, false);
        }
      }
    }
  };

  auto root = json.move_as_ok();
  td::uint64 expected_peers_count = 0;
  if (root.type() == td::JsonValue::Type::Array) {
    for (const auto &item : root.get_array()) {
      load_peer(item);
    }
  } else if (root.type() == td::JsonValue::Type::Object) {
    const auto &obj = root.get_object();
    auto peers_count = obj.get_optional_long_field("peers_count", 0);
    if (peers_count.is_ok()) {
      expected_peers_count = static_cast<td::uint64>(std::max<td::int64>(0, peers_count.move_as_ok()));
    }
    auto fec_parts_received = obj.get_optional_long_field("fec_parts_received", 0);
    if (fec_parts_received.is_ok()) {
      overlay_observer_fec_parts_received_ =
          static_cast<td::uint64>(std::max<td::int64>(0, fec_parts_received.move_as_ok()));
    }
    obj.foreach([&](td::Slice field_name, const td::JsonValue &field_value) {
      if (field_name == "peers" && field_value.type() == td::JsonValue::Type::Array) {
        for (const auto &item : field_value.get_array()) {
          load_peer(item);
        }
      }
    });
  } else {
    LOG(WARNING) << "full-node overlay observer peers root is neither object nor array: " << overlay_observer_peers_path_;
    return false;
  }
  if (expected_peers_count > 0 && overlay_observer_members_.empty()) {
    LOG(ERROR) << "refusing to accept empty full-node overlay observer state loaded from non-empty "
               << overlay_observer_peers_path_ << " expected_peers_count=" << expected_peers_count;
    return false;
  }

  LOG(WARNING) << "full-node overlay observer loaded " << overlay_observer_members_.size() << " peers and "
               << overlay_observer_fec_senders_.size() << " FEC senders from " << overlay_observer_peers_path_;
  return true;
}

bool FullNodeImpl::add_overlay_observer_peer(overlay::OverlayIdShort overlay_id, adnl::AdnlNodeIdShort peer,
                                             bool enqueue, bool dirty, td::int32 version, td::uint32 flags) {
  (void)enqueue;
  if (!opts_.overlay_observer_.enabled_ || peer == adnl_id_) {
    return false;
  }
  auto &known = overlay_observer_known_peers_[overlay_id];
  bool new_known_peer = known.insert(peer).second;
  if (new_known_peer) {
    overlay_observer_initial_peers_[overlay_id].push_back(peer);
  }

  auto peer_hex = observer_adnl_id_hex(peer);
  auto &member = overlay_observer_members_[peer_hex];
  bool new_member = member.adnl_id.empty();
  if (new_member) {
    member.adnl_id = peer_hex;
  }
  auto overlay_hex = observer_overlay_id_hex(overlay_id);
  auto shard_it = overlay_observer_overlay_shards_.find(overlay_id);
  auto shard_str = shard_it == overlay_observer_overlay_shards_.end() ? std::string() : shard_it->second.to_str();
  if (member.last_overlay.empty()) {
    member.last_overlay = overlay_hex;
  }
  if (member.last_shard.empty() && !shard_str.empty()) {
    member.last_shard = shard_str;
  }
  bool overlay_updated = false;
  auto overlay_it = member.overlays.find(overlay_hex);
  if (overlay_it == member.overlays.end()) {
    member.overlays.emplace(overlay_hex, shard_str);
    overlay_updated = true;
  } else if (overlay_it->second.empty() && !shard_str.empty()) {
    overlay_it->second = shard_str;
    overlay_updated = true;
  }
  bool metadata_updated = false;
  if (version != 0 && version >= member.version) {
    metadata_updated = member.version != version || member.flags != flags;
    if (metadata_updated) {
      member.version = version;
      member.flags = flags;
    }
  }
  if (dirty || new_member || metadata_updated || overlay_updated) {
    overlay_observer_members_dirty_ = true;
  }
  return new_member;
}

bool FullNodeImpl::overlay_observer_can_query_peer(overlay::OverlayIdShort overlay_id,
                                                   adnl::AdnlNodeIdShort peer, bool require_address) const {
  if (!opts_.overlay_observer_.enabled_ || peer == adnl_id_) {
    return false;
  }
  if (!overlay_observer_overlay_shards_.contains(overlay_id)) {
    return false;
  }
  auto member_it = overlay_observer_members_.find(observer_adnl_id_hex(peer));
  if (member_it == overlay_observer_members_.end()) {
    return false;
  }
  if (require_address && member_it->second.addresses.empty()) {
    return false;
  }
  return true;
}

bool FullNodeImpl::queue_overlay_observer_member_queries(adnl::AdnlNodeIdShort peer, td::uint32 count, bool front,
                                                         bool require_address) {
  if (!opts_.overlay_observer_.enabled_) {
    return false;
  }
  auto peer_hex = observer_adnl_id_hex(peer);
  auto member_it = overlay_observer_members_.find(peer_hex);
  if (member_it == overlay_observer_members_.end() || count == 0) {
    return false;
  }

  std::vector<std::string> overlay_hexes;
  if (!member_it->second.last_overlay.empty()) {
    overlay_hexes.push_back(member_it->second.last_overlay);
  }
  for (const auto &[overlay_hex, _] : member_it->second.overlays) {
    if (overlay_hex != member_it->second.last_overlay) {
      overlay_hexes.push_back(overlay_hex);
    }
  }
  for (const auto &overlay_hex : overlay_hexes) {
    auto overlay_id = observer_parse_overlay_hex(overlay_hex);
    if (overlay_id.is_error()) {
      continue;
    }
    auto oid = overlay_id.move_as_ok();
    if (!overlay_observer_can_query_peer(oid, peer, require_address)) {
      continue;
    }
    queue_overlay_observer_target(oid, peer, count, front, require_address);
    return true;
  }
  return false;
}

bool FullNodeImpl::queue_overlay_observer_probe_queries(adnl::AdnlNodeIdShort peer, bool front) {
  auto peer_hex = observer_adnl_id_hex(peer);
  auto member_it = overlay_observer_members_.find(peer_hex);
  if (member_it == overlay_observer_members_.end()) {
    return false;
  }
  auto now = td::Clocks::system();
  if (member_it->second.last_request_at != 0.0 &&
      now - member_it->second.last_request_at < OVERLAY_OBSERVER_NO_ADDRESS_PROBE_MIN_INTERVAL) {
    return false;
  }
  if (!queue_overlay_observer_member_queries(peer, OVERLAY_OBSERVER_NO_ADDRESS_PROBE_QUERIES, front,
                                             false /* require_address */)) {
    return false;
  }
  member_it->second.last_request_at = now;
  overlay_observer_members_dirty_ = true;
  return true;
}

bool FullNodeImpl::overlay_observer_peer_is_recent(const std::string &peer_hex, double now) const {
  auto member_it = overlay_observer_members_.find(peer_hex);
  if (member_it == overlay_observer_members_.end()) {
    return false;
  }
  auto min_version = now - static_cast<double>(2 * overlay::Overlays::overlay_peer_ttl());
  const auto &member = member_it->second;
  if (member.version > 0 && static_cast<double>(member.version) >= min_version) {
    return true;
  }
  auto fec_sender_it = overlay_observer_fec_senders_.find(peer_hex);
  return fec_sender_it != overlay_observer_fec_senders_.end() && fec_sender_it->second.last_seen >= min_version;
}

bool FullNodeImpl::choose_overlay_observer_oldest_target(OverlayObserverTarget &target) const {
  auto now = td::Clocks::system();
  bool found = false;
  double best_query_at = 0.0;
  for (const auto &[overlay_id, peers] : overlay_observer_initial_peers_) {
    if (!overlay_observer_overlay_shards_.contains(overlay_id)) {
      continue;
    }
    for (const auto &peer : peers) {
      if (!overlay_observer_can_query_peer(overlay_id, peer, false /* require_address */)) {
        continue;
      }
      if (overlay_observer_active_query_targets_.contains(observer_overlay_target_key(overlay_id, peer))) {
        continue;
      }
      auto peer_hex = observer_adnl_id_hex(peer);
      auto member_it = overlay_observer_members_.find(peer_hex);
      if (member_it == overlay_observer_members_.end()) {
        continue;
      }
      const auto &member = member_it->second;
      if (!overlay_observer_peer_is_recent(peer_hex, now)) {
        continue;
      }
      auto last_query_at = std::max(member.last_request_at, std::max(member.last_success_at, member.last_fail_at));
      if (!found || last_query_at < best_query_at) {
        target = OverlayObserverTarget{overlay_id, peer};
        best_query_at = last_query_at;
        found = true;
      }
    }
  }
  return found;
}

void FullNodeImpl::queue_overlay_observer_target(overlay::OverlayIdShort overlay_id, adnl::AdnlNodeIdShort peer,
                                                 td::uint32 count, bool front, bool require_address) {
  if (!overlay_observer_can_query_peer(overlay_id, peer, require_address) || count == 0) {
    return;
  }
  OverlayObserverTarget target{overlay_id, peer};
  for (td::uint32 i = 0; i < count; ++i) {
    if (front) {
      overlay_observer_queue_.push_front(target);
    } else {
      overlay_observer_queue_.push_back(target);
    }
  }
}

bool FullNodeImpl::choose_overlay_observer_oldest_address_target(adnl::AdnlNodeIdShort &target) const {
  auto now = td::Clocks::system();
  bool found = false;
  double best_query_at = 0.0;
  for (const auto &[peer_hex, member] : overlay_observer_members_) {
    if (!overlay_observer_peer_is_recent(peer_hex, now)) {
      continue;
    }
    if (overlay_observer_active_address_resolves_.contains(peer_hex)) {
      continue;
    }
    auto peer = observer_parse_adnl_hex(peer_hex);
    if (peer.is_error()) {
      continue;
    }
    auto last_dht_query_at = std::max(member.last_dht_success_at, member.last_dht_fail_at);
    if (!found || last_dht_query_at < best_query_at) {
      target = peer.move_as_ok();
      best_query_at = last_dht_query_at;
      found = true;
    }
  }
  return found;
}

void FullNodeImpl::pump_overlay_observer_address_resolves() {
  if (!opts_.overlay_observer_.enabled_ || dht_.empty()) {
    return;
  }
  while (overlay_observer_active_address_resolves_.size() < OVERLAY_OBSERVER_MAX_ACTIVE_ADDRESS_RESOLVES) {
    adnl::AdnlNodeIdShort id;
    if (!choose_overlay_observer_oldest_address_target(id)) {
      break;
    }
    auto peer_hex = observer_adnl_id_hex(id);
    auto &member = overlay_observer_members_[peer_hex];
    if (member.adnl_id.empty()) {
      member.adnl_id = peer_hex;
    }
    overlay_observer_active_address_resolves_.insert(peer_hex);
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), id](td::Result<dht::DhtValue> R) {
      td::actor::send_closure(SelfId, &FullNodeImpl::finish_overlay_observer_address_resolve, id, std::move(R));
    });
    td::actor::send_closure(dht_, &dht::Dht::get_value, dht::DhtKey{id.pubkey_hash(), "address", 0}, std::move(P));
  }
}

std::vector<FullNodeImpl::OverlayObserverAddress> FullNodeImpl::extract_overlay_observer_addresses(
    const adnl::AdnlAddressList &addr_list) const {
  std::vector<OverlayObserverAddress> addresses;
  auto tl = addr_list.tl();
  for (const auto &addr : tl->addrs_) {
    ton_api::downcast_call(
        *addr, td::overloaded(
                   [&](const ton_api::adnl_address_udp &obj) {
                     addresses.push_back({"udp", td::IPAddress::ipv4_to_str(obj.ip_), obj.port_});
                   },
                   [&](const ton_api::adnl_address_udp6 &obj) {
                     addresses.push_back({"udp6", td::IPAddress::ipv6_to_str(obj.ip_.as_slice()), obj.port_});
                   },
                   [&](const ton_api::adnl_address_quic &obj) {
                     addresses.push_back({"quic", td::IPAddress::ipv4_to_str(obj.ip_), obj.port_});
                   },
                   [&](const auto &) {}));
  }
  return addresses;
}

bool FullNodeImpl::apply_overlay_observer_addresses(adnl::AdnlNodeIdShort peer, adnl::AdnlNodeIdFull full_id,
                                                    adnl::AdnlAddressList addr_list, std::string source,
                                                    std::string *error) {
  if (full_id.compute_short_id() != peer || !full_id.pubkey().is_ed25519()) {
    if (error != nullptr) {
      *error = "address value has mismatching public key";
    }
    return false;
  }
  auto peer_hex = observer_adnl_id_hex(peer);
  auto &member = overlay_observer_members_[peer_hex];
  if (member.adnl_id.empty()) {
    member.adnl_id = peer_hex;
  }
  auto addresses = extract_overlay_observer_addresses(addr_list);
  td::actor::send_closure(adnl_, &adnl::Adnl::add_peer, adnl_id_, full_id, addr_list);
  if (addresses.empty()) {
    if (error != nullptr) {
      *error = "address list has no usable udp/quic addresses";
    }
    return false;
  }
  member.addresses = std::move(addresses);
  member.address_updated_at = td::Clocks::system();
  member.address_source = std::move(source);
  member.success_query_but_no_ip_at = 0.0;
  return true;
}

td::Result<FullNodeImpl::OverlayObserverAddress> FullNodeImpl::parse_overlay_observer_connection_endpoint(
    td::Slice endpoint) const {
  auto value = endpoint.str();
  if (value.empty() || value == "undefined" || value == "tunnel") {
    return td::Status::Error("connection endpoint is not a UDP endpoint");
  }
  auto pos = value.rfind(':');
  if (pos == std::string::npos || pos == 0 || pos + 1 >= value.size()) {
    return td::Status::Error("connection endpoint has no port");
  }
  auto ip = value.substr(0, pos);
  auto port = td::to_integer_safe<td::int32>(td::Slice(value).substr(pos + 1));
  if (port.is_error()) {
    return port.move_as_error_prefix("bad connection endpoint port: ");
  }
  auto port_value = port.move_as_ok();
  if (port_value <= 0 || port_value > 65535) {
    return td::Status::Error("connection endpoint port is out of range");
  }
  OverlayObserverAddress address;
  address.type = ip.find(':') == std::string::npos ? "udp" : "udp6";
  address.ip = std::move(ip);
  address.port = port_value;
  return address;
}

bool FullNodeImpl::apply_overlay_observer_connection_endpoint(adnl::AdnlNodeIdShort peer, td::Slice endpoint,
                                                              double ts) {
  auto parsed = parse_overlay_observer_connection_endpoint(endpoint);
  if (parsed.is_error()) {
    return false;
  }
  auto address = parsed.move_as_ok();
  auto peer_hex = observer_adnl_id_hex(peer);
  auto &member = overlay_observer_members_[peer_hex];
  if (member.adnl_id.empty()) {
    member.adnl_id = peer_hex;
  }
  if (!member.addresses.empty() && member.address_source != "connection") {
    return false;
  }

  bool changed = member.address_source != "connection" || member.addresses.size() != 1 ||
                 member.addresses[0].type != address.type || member.addresses[0].ip != address.ip ||
                 member.addresses[0].port != address.port;
  member.addresses.clear();
  member.addresses.push_back(std::move(address));
  member.address_updated_at = ts;
  member.address_source = "connection";
  member.success_query_but_no_ip_at = 0.0;
  overlay_observer_members_dirty_ = true;
  return changed;
}

void FullNodeImpl::log_overlay_observer_address_resolve(td::Slice source, double ts, td::Slice peer_hex, bool success,
                                                        size_t address_count, size_t active_resolves, td::Slice error) {
  if (!overlay_observer_members_log_stream_) {
    return;
  }
  overlay_observer_members_log_stream_ << std::fixed << std::setprecision(6)
                                       << "{\"event\":\"overlay_address_resolve\",\"source\":"
                                       << observer_json_quote(source) << ",\"ts\":" << ts
                                       << ",\"target_adnl_id\":" << observer_json_quote(peer_hex)
                                       << ",\"success\":" << (success ? "true" : "false")
                                       << ",\"addresses\":" << address_count
                                       << ",\"active_address_resolves\":" << active_resolves;
  if (!error.empty()) {
    overlay_observer_members_log_stream_ << ",\"error\":" << observer_json_quote(error);
  }
  overlay_observer_members_log_stream_ << "}\n";
  overlay_observer_members_log_stream_.flush();
}

void FullNodeImpl::finish_overlay_observer_address_resolve(adnl::AdnlNodeIdShort peer,
                                                           td::Result<dht::DhtValue> result) {
  auto peer_hex = observer_adnl_id_hex(peer);
  overlay_observer_active_address_resolves_.erase(peer_hex);

  auto &member = overlay_observer_members_[peer_hex];
  if (member.adnl_id.empty()) {
    member.adnl_id = peer_hex;
  }
  auto now = td::Clocks::system();
  std::string error;
  bool success = false;
  adnl::AdnlNodeIdFull full_id;
  adnl::AdnlAddressList addr_list;

  if (result.is_error()) {
    error = result.move_as_error().message().str();
  } else {
    auto value = result.move_as_ok();
    full_id = adnl::AdnlNodeIdFull{value.key().public_key()};
    if (full_id.compute_short_id() != peer || !full_id.pubkey().is_ed25519()) {
      error = "DHT address value has mismatching public key";
    } else {
      auto addr_list_tl = fetch_tl_object<ton_api::adnl_addressList>(value.value().clone(), true);
      if (addr_list_tl.is_error()) {
        error = PSTRING() << "parse failed: " << addr_list_tl.move_as_error();
      } else {
        auto parsed_addr_list = adnl::AdnlAddressList::create(addr_list_tl.move_as_ok());
        if (parsed_addr_list.is_error()) {
          error = parsed_addr_list.move_as_error().message().str();
        } else {
          addr_list = parsed_addr_list.move_as_ok();
          success = true;
        }
      }
    }
  }

  if (success) {
    success = apply_overlay_observer_addresses(peer, std::move(full_id), std::move(addr_list), "dht", &error);
  }
  if (success) {
    member.last_dht_success_at = now;
    member.dht_successes++;
    member.last_dht_error.clear();
  } else {
    member.last_dht_fail_at = now;
    member.dht_failures++;
    member.last_dht_error = std::move(error);
  }
  overlay_observer_members_dirty_ = true;

  log_overlay_observer_address_resolve("dht", now, peer_hex, success, member.addresses.size(),
                                       overlay_observer_active_address_resolves_.size(), member.last_dht_error);

  pump_overlay_observer_address_resolves();
  pump_overlay_observer_queries();
}

void FullNodeImpl::lookup_overlay_observer_success_address(adnl::AdnlNodeIdShort peer, double ts) {
  auto peer_hex = observer_adnl_id_hex(peer);
  auto &member = overlay_observer_members_[peer_hex];
  if (member.adnl_id.empty()) {
    member.adnl_id = peer_hex;
  }
  if (!member.addresses.empty()) {
    return;
  }

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), peer, ts](td::Result<adnl::Adnl::PeerAddrLists> R) {
    td::actor::send_closure(SelfId, &FullNodeImpl::finish_overlay_observer_success_peer_table_address, peer, ts,
                            std::move(R));
  });
  td::actor::send_closure(adnl_, &adnl::Adnl::get_peer_addr_lists, adnl_id_, peer, std::move(P));
}

void FullNodeImpl::finish_overlay_observer_success_peer_table_address(
    adnl::AdnlNodeIdShort peer, double ts, td::Result<adnl::Adnl::PeerAddrLists> result) {
  auto peer_hex = observer_adnl_id_hex(peer);
  auto &member = overlay_observer_members_[peer_hex];
  if (member.adnl_id.empty()) {
    member.adnl_id = peer_hex;
  }
  if (!member.addresses.empty()) {
    return;
  }

  std::string error;
  bool success = false;
  if (result.is_error()) {
    error = result.move_as_error().message().str();
  } else {
    auto addr_lists = result.move_as_ok();
    if (!addr_lists.addr_list.empty()) {
      success = apply_overlay_observer_addresses(peer, addr_lists.pub_id, std::move(addr_lists.addr_list), "overlay",
                                                 &error);
    } else if (!addr_lists.priority_addr_list.empty()) {
      success = apply_overlay_observer_addresses(peer, addr_lists.pub_id, std::move(addr_lists.priority_addr_list),
                                                 "overlay", &error);
    } else {
      error = "ADNL peer table has no normal or priority address list";
    }
  }

  if (success) {
    overlay_observer_members_dirty_ = true;
    log_overlay_observer_address_resolve("overlay", ts, peer_hex, true, member.addresses.size(), 0);
    pump_overlay_observer_queries();
    return;
  }

  overlay_observer_members_dirty_ = true;
  log_overlay_observer_address_resolve("overlay", ts, peer_hex, false, member.addresses.size(), 0, error);

  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), peer, ts, peer_table_error = std::move(error)](td::Result<td::string> R) mutable {
        td::actor::send_closure(SelfId, &FullNodeImpl::finish_overlay_observer_success_connection_address, peer, ts,
                                std::move(peer_table_error), std::move(R));
      });
  td::actor::send_closure(adnl_, &adnl::Adnl::get_conn_ip_str, adnl_id_, peer, std::move(P));
}

void FullNodeImpl::finish_overlay_observer_success_connection_address(adnl::AdnlNodeIdShort peer, double ts,
                                                                      std::string peer_table_error,
                                                                      td::Result<td::string> result) {
  auto peer_hex = observer_adnl_id_hex(peer);
  auto &member = overlay_observer_members_[peer_hex];
  if (member.adnl_id.empty()) {
    member.adnl_id = peer_hex;
  }
  if (!member.addresses.empty()) {
    return;
  }

  std::string endpoint;
  std::string error;
  if (result.is_error()) {
    error = result.move_as_error().message().str();
  } else {
    endpoint = result.move_as_ok();
    if (endpoint.empty() || endpoint == "undefined" || endpoint == "tunnel") {
      error = PSTRING() << "connection endpoint is " << (endpoint.empty() ? std::string("empty") : endpoint);
    } else {
      bool endpoint_changed = member.last_conn_ip != endpoint;
      member.last_conn_ip = endpoint;
      member.last_conn_ip_at = ts;
      bool address_changed = apply_overlay_observer_connection_endpoint(peer, endpoint, ts);
      if (overlay_observer_members_log_stream_ && (endpoint_changed || address_changed)) {
        overlay_observer_members_log_stream_ << std::fixed << std::setprecision(6)
                                             << "{\"event\":\"overlay_connection_endpoint\""
                                             << ",\"ts\":" << ts
                                             << ",\"target_adnl_id\":" << observer_json_quote(peer_hex)
                                             << ",\"conn_ip\":" << observer_json_quote(endpoint)
                                             << ",\"address_source\":" << observer_json_quote(member.address_source)
                                             << ",\"address_updated\":" << (address_changed ? "true" : "false")
                                             << "}\n";
        overlay_observer_members_log_stream_.flush();
      }
      overlay_observer_members_dirty_ = true;
      if (!member.addresses.empty()) {
        pump_overlay_observer_queries();
        return;
      }
      error = "connection endpoint did not produce an address";
    }
  }

  member.success_query_but_no_ip_at = ts;
  overlay_observer_members_dirty_ = true;
  if (overlay_observer_members_log_stream_) {
    overlay_observer_members_log_stream_ << std::fixed << std::setprecision(6)
                                         << "{\"event\":\"overlay_success_query_but_no_ip\""
                                         << ",\"ts\":" << ts
                                         << ",\"target_adnl_id\":" << observer_json_quote(peer_hex)
                                         << ",\"peer_table_error\":" << observer_json_quote(peer_table_error)
                                         << ",\"connection_error\":" << observer_json_quote(error)
                                         << "}\n";
    overlay_observer_members_log_stream_.flush();
  }
  pump_overlay_observer_queries();
}

void FullNodeImpl::dump_overlay_observer_peer_state() {
  if (!opts_.overlay_observer_.enabled_ || overlay_observer_peers_path_.empty()) {
    return;
  }
  std::set<std::string> peer_ids;
  for (const auto &[peer_id, _] : overlay_observer_members_) {
    peer_ids.insert(peer_id);
  }
  for (const auto &[peer_id, _] : overlay_observer_fec_senders_) {
    peer_ids.insert(peer_id);
  }

  std::ostringstream os;
  os << std::fixed << std::setprecision(6);
  os << "{\n  \"updated_at\": " << td::Clocks::system() << ",\n  \"peers_count\": " << peer_ids.size()
     << ",\n  \"fec_senders_count\": " << overlay_observer_fec_senders_.size()
     << ",\n  \"fec_parts_received\": " << overlay_observer_fec_parts_received_ << ",\n  \"peers\": [\n";

  bool first = true;
  for (const auto &peer_id : peer_ids) {
    if (!first) {
      os << ",\n";
    }
    first = false;

    OverlayObserverMember default_member;
    default_member.adnl_id = peer_id;
    const auto member_it = overlay_observer_members_.find(peer_id);
    const auto &member = member_it == overlay_observer_members_.end() ? default_member : member_it->second;
    os << "    {\"adnl_id\": " << observer_json_quote(peer_id) << ", \"version\": " << member.version
       << ", \"flags\": " << member.flags << ", \"first_request_at\": " << member.first_request_at
       << ", \"last_request_at\": " << member.last_request_at
       << ", \"last_response_at\": " << member.last_response_at << ", \"last_latency\": " << member.last_latency
       << ", \"last_success_at\": " << member.last_success_at << ", \"last_fail_at\": " << member.last_fail_at
       << ", \"requests\": " << member.requests << ", \"successes\": " << member.successes
       << ", \"failures\": " << member.failures << ", \"consecutive_failures\": " << member.consecutive_failures
       << ", \"last_returned_peers\": " << member.last_returned_peers
       << ", \"max_returned_peers\": " << member.max_returned_peers
       << ", \"last_overlay\": " << observer_json_quote(member.last_overlay)
       << ", \"last_shard\": " << observer_json_quote(member.last_shard)
       << ", \"overlays\": [";
    bool first_overlay = true;
    for (const auto &[overlay, shard] : member.overlays) {
      if (!first_overlay) {
        os << ", ";
      }
      first_overlay = false;
      os << "{\"overlay\": " << observer_json_quote(overlay) << ", \"shard\": " << observer_json_quote(shard) << "}";
    }
    os << "]";
    os << ", \"last_error\": " << observer_json_quote(member.last_error) << ", \"neighbours\": [";
    bool first_neighbour = true;
    for (const auto &neighbour : member.neighbours) {
      if (!first_neighbour) {
        os << ", ";
      }
      first_neighbour = false;
      os << observer_json_quote(neighbour);
    }
    os << "]";
    os << ", \"recent_neighbours\": [";
    bool first_recent_neighbour = true;
    for (const auto &neighbour : member.recent_neighbours) {
      if (neighbour == peer_id) {
        continue;
      }
      if (!first_recent_neighbour) {
        os << ", ";
      }
      first_recent_neighbour = false;
      os << observer_json_quote(neighbour);
    }
    os << "]";
    os << ", \"addresses\": [";
    bool first_address = true;
    for (const auto &address : member.addresses) {
      if (!first_address) {
        os << ", ";
      }
      first_address = false;
      os << "{\"type\": " << observer_json_quote(address.type) << ", \"ip\": " << observer_json_quote(address.ip)
         << ", \"port\": " << address.port << "}";
    }
    os << "]";
    if (member.address_updated_at != 0.0) {
      os << ", \"address_updated_at\": " << member.address_updated_at;
    }
    if (!member.address_source.empty()) {
      os << ", \"address_source\": " << observer_json_quote(member.address_source);
    }
    if (member.last_dht_success_at != 0.0) {
      os << ", \"last_dht_success_at\": " << member.last_dht_success_at;
    }
    if (member.last_dht_fail_at != 0.0) {
      os << ", \"last_dht_fail_at\": " << member.last_dht_fail_at;
    }
    if (member.dht_successes != 0) {
      os << ", \"dht_successes\": " << member.dht_successes;
    }
    if (member.dht_failures != 0) {
      os << ", \"dht_failures\": " << member.dht_failures;
    }
    if (!member.last_dht_error.empty()) {
      os << ", \"last_dht_error\": " << observer_json_quote(member.last_dht_error);
    }
    if (!member.last_conn_ip.empty()) {
      os << ", \"last_conn_ip\": " << observer_json_quote(member.last_conn_ip);
    }
    if (member.last_conn_ip_at != 0.0) {
      os << ", \"last_conn_ip_at\": " << member.last_conn_ip_at;
    }
    if (member.success_query_but_no_ip_at != 0.0) {
      os << ", \"success_query_but_no_ip_at\": " << member.success_query_but_no_ip_at;
    }

    const auto sender_it = overlay_observer_fec_senders_.find(peer_id);
    if (sender_it != overlay_observer_fec_senders_.end()) {
      const auto &stat = sender_it->second;
      os << ", \"fec\": {\"first_seen\": " << stat.first_seen << ", \"last_seen\": " << stat.last_seen
         << ", \"fec_parts\": " << stat.fec_parts;
      if (!stat.last_type.empty()) {
        os << ", \"last_type\": " << observer_json_quote(stat.last_type);
      }
      if (!stat.last_origin_broadcaster_adnl_id.empty()) {
        os << ", \"last_origin_broadcaster_adnl_id\": " << observer_json_quote(stat.last_origin_broadcaster_adnl_id);
      }
      if (!stat.last_source_key_id.empty()) {
        os << ", \"last_source_key_id\": " << observer_json_quote(stat.last_source_key_id);
      }
      if (!stat.last_data_hash.empty()) {
        os << ", \"last_data_hash\": " << observer_json_quote(stat.last_data_hash);
      }
      if (!stat.last_part_hash.empty()) {
        os << ", \"last_part_hash\": " << observer_json_quote(stat.last_part_hash);
      }
      if (!stat.last_part_data_hash.empty()) {
        os << ", \"last_part_data_hash\": " << observer_json_quote(stat.last_part_data_hash);
      }
      if (!stat.last_broadcast_hash.empty()) {
        os << ", \"last_broadcast_hash\": " << observer_json_quote(stat.last_broadcast_hash);
      }
      if (stat.has_last_seqno) {
        os << ", \"last_seqno\": " << stat.last_seqno;
      }
      os << ", \"parts_by_type\": {";
      bool first_type = true;
      for (const auto &[type, count] : stat.parts_by_type) {
        if (!first_type) {
          os << ", ";
        }
        first_type = false;
        os << observer_json_quote(type) << ": " << count;
      }
      os << "}}";
    }
    os << "}";
  }

  os << "\n  ]\n}\n";
  if (td::stat(overlay_observer_peers_path_).is_ok()) {
    auto existing_data = td::read_file(overlay_observer_peers_path_);
    if (existing_data.is_error()) {
      LOG(ERROR) << "refusing to overwrite " << overlay_observer_peers_path_
                 << " because existing state could not be read: " << existing_data.move_as_error();
      return;
    }
    auto existing_data_buf = existing_data.move_as_ok();
    auto existing_json = td::json_decode(existing_data_buf.as_slice());
    if (existing_json.is_error()) {
      LOG(ERROR) << "refusing to overwrite " << overlay_observer_peers_path_
                 << " because existing state could not be parsed: " << existing_json.move_as_error();
      return;
    }
    auto existing_root = existing_json.move_as_ok();
    if (existing_root.type() != td::JsonValue::Type::Object) {
      LOG(ERROR) << "refusing to overwrite " << overlay_observer_peers_path_
                 << " because existing state root is not an object";
      return;
    }
    auto existing_peers_count = existing_root.get_object().get_optional_long_field("peers_count", 0);
    if (existing_peers_count.is_error()) {
      LOG(ERROR) << "refusing to overwrite " << overlay_observer_peers_path_
                 << " because existing peers_count could not be read: " << existing_peers_count.move_as_error();
      return;
    }
    auto existing_count = static_cast<td::uint64>(std::max<td::int64>(0, existing_peers_count.move_as_ok()));
    if (existing_count > peer_ids.size() &&
        existing_count - peer_ids.size() >= OVERLAY_OBSERVER_MIN_PEER_SHRINK_TO_REFUSE) {
      LOG(ERROR) << "refusing to overwrite " << overlay_observer_peers_path_ << " with fewer peers: existing="
                 << existing_count << " new=" << peer_ids.size();
      return;
    }
  }
  auto S = td::atomic_write_file(overlay_observer_peers_path_, os.str());
  if (S.is_error()) {
    LOG(WARNING) << "failed to write " << overlay_observer_peers_path_ << ": " << S;
  } else {
    overlay_observer_fec_senders_dirty_ = false;
    overlay_observer_members_dirty_ = false;
  }
}

void FullNodeImpl::seed_overlay_observer_peers(ShardIdFull shard, ShardInfo &info) {
  if (!opts_.overlay_observer_.enabled_ || info.actor.empty() || info.overlay_observer_seeded) {
    return;
  }
  auto hash = create_hash_tl_object<ton_api::tonNode_shardPublicOverlayId>(shard.workchain, shard.shard,
                                                                           zero_state_file_hash_);
  td::BufferSlice bytes{32};
  bytes.as_slice().copy_from(as_slice(hash));
  auto overlay_id = overlay::OverlayIdFull{std::move(bytes)}.compute_short_id();
  overlay_observer_overlay_shards_[overlay_id] = shard;
  auto it = overlay_observer_initial_peers_.find(overlay_id);
  if (it == overlay_observer_initial_peers_.end() || it->second.empty()) {
    return;
  }
  info.overlay_observer_seeded = true;
  LOG(WARNING) << "full-node overlay observer loaded " << it->second.size() << " target peers for shard "
               << shard.to_str() << " overlay=" << observer_overlay_id_hex(overlay_id);
  rebuild_overlay_observer_queue();
}

void FullNodeImpl::rebuild_overlay_observer_queue() {
  if (!opts_.overlay_observer_.enabled_) {
    return;
  }
  pump_overlay_observer_address_resolves();
  pump_overlay_observer_queries();
}

void FullNodeImpl::pump_overlay_observer_queries() {
  if (!opts_.overlay_observer_.enabled_) {
    return;
  }
  auto max_active = std::max<td::uint32>(1, opts_.overlay_observer_.max_active_queries_);
  while (overlay_observer_active_queries_ < max_active) {
    OverlayObserverTarget target;
    bool have_target = false;
    if (!overlay_observer_queue_.empty()) {
      auto queue_size = overlay_observer_queue_.size();
      for (size_t i = 0; i < queue_size; ++i) {
        auto queued_target = overlay_observer_queue_.front();
        overlay_observer_queue_.pop_front();
        if (!overlay_observer_can_query_peer(queued_target.first, queued_target.second, false /* require_address */)) {
          continue;
        }
        if (overlay_observer_active_query_targets_.contains(
                observer_overlay_target_key(queued_target.first, queued_target.second))) {
          overlay_observer_queue_.push_back(queued_target);
          continue;
        }
        target = queued_target;
        have_target = true;
        break;
      }
    }
    if (!have_target) {
      have_target = choose_overlay_observer_oldest_target(target);
    }
    if (!have_target) {
      break;
    }
    auto target_key = observer_overlay_target_key(target.first, target.second);
    if (!overlay_observer_active_query_targets_.insert(target_key).second) {
      continue;
    }
    overlay_observer_active_queries_++;
    send_overlay_observer_query(target.first, target.second);
  }
}

void FullNodeImpl::send_overlay_observer_query(overlay::OverlayIdShort overlay_id, adnl::AdnlNodeIdShort target) {
  auto request_at = td::Clocks::system();
  auto target_hex = observer_adnl_id_hex(target);
  auto &member = overlay_observer_members_[target_hex];
  if (member.adnl_id.empty()) {
    member.adnl_id = target_hex;
  }
  if (member.first_request_at == 0.0) {
    member.first_request_at = request_at;
  }
  member.last_request_at = request_at;
  overlay_observer_members_dirty_ = true;
  overlay::OverlayNode self_node{adnl_id_, overlay_id, 0};
  auto to_sign = self_node.to_sign();
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), overlay_id, target, self_node = std::move(self_node), request_at](
          td::Result<std::pair<td::BufferSlice, PublicKey>> R) mutable {
        if (R.is_error()) {
          td::actor::send_closure(SelfId, &FullNodeImpl::finish_overlay_observer_query_error, overlay_id, target,
                                  request_at, PSTRING() << "sign failed: " << R.move_as_error());
          return;
        }
        auto pair = R.move_as_ok();
        self_node.update_signature(pair.first.as_slice());
        self_node.update_adnl_id(adnl::AdnlNodeIdFull{pair.second});
        td::actor::send_closure(SelfId, &FullNodeImpl::send_signed_overlay_observer_query, overlay_id, target,
                                std::move(self_node), request_at);
      });
  td::actor::send_closure(keyring_, &keyring::Keyring::sign_add_get_public_key, adnl_id_.pubkey_hash(),
                          std::move(to_sign), std::move(P));
}

void FullNodeImpl::send_signed_overlay_observer_query(overlay::OverlayIdShort overlay_id, adnl::AdnlNodeIdShort target,
                                                      overlay::OverlayNode self_node, double request_at) {
  std::vector<tl_object_ptr<ton_api::overlay_node>> nodes;
  nodes.push_back(self_node.tl());
  auto query = create_serialize_tl_object<ton_api::overlay_getRandomPeers>(
      create_tl_object<ton_api::overlay_nodes>(std::move(nodes)));
  auto P = td::PromiseCreator::lambda(
      [SelfId = actor_id(this), overlay_id, target, request_at](td::Result<td::BufferSlice> R) mutable {
        td::actor::send_closure(SelfId, &FullNodeImpl::finish_overlay_observer_query, overlay_id, target, request_at,
                                std::move(R));
      });
  overlay_observer_queries_++;
  td::actor::send_closure(overlays_, &overlay::Overlays::send_query, target, adnl_id_, overlay_id,
                          "full-node observer getRandomPeers", std::move(P),
                          td::Timestamp::in(opts_.overlay_observer_.query_timeout_), std::move(query));
}

void FullNodeImpl::finish_overlay_observer_query_error(overlay::OverlayIdShort overlay_id, adnl::AdnlNodeIdShort target,
                                                       double request_at, std::string error) {
  finish_overlay_observer_query(overlay_id, target, request_at, td::Status::Error(std::move(error)));
}

void FullNodeImpl::finish_overlay_observer_query(overlay::OverlayIdShort overlay_id, adnl::AdnlNodeIdShort target,
                                                 double request_at, td::Result<td::BufferSlice> result) {
  auto response_at = td::Clocks::system();
  overlay_observer_active_query_targets_.erase(observer_overlay_target_key(overlay_id, target));
  if (overlay_observer_active_queries_ > 0) {
    overlay_observer_active_queries_--;
  }

  bool success = false;
  td::uint32 returned_peers = 0;
  td::uint32 valid_returned_peers = 0;
  td::uint32 new_peers = 0;
  std::string error;
  std::vector<overlay::OverlayNode> returned_nodes;
  std::vector<std::string> returned_peer_ids;
  std::vector<std::string> new_peer_ids;
  if (result.is_error()) {
    error = result.move_as_error().message().str();
  } else {
    auto nodes = fetch_tl_object<ton_api::overlay_nodes>(result.move_as_ok(), true);
    if (nodes.is_error()) {
      error = PSTRING() << "parse failed: " << nodes.move_as_error();
    } else {
      success = true;
      auto nodes_obj = nodes.move_as_ok();
      returned_peers = static_cast<td::uint32>(nodes_obj->nodes_.size());
      for (const auto &node_tl : nodes_obj->nodes_) {
        auto N = overlay::OverlayNode::create(node_tl);
        if (N.is_error()) {
          continue;
        }
        auto node = N.move_as_ok();
        if (node.overlay_id() != overlay_id || node.adnl_id_short() == adnl_id_) {
          continue;
        }
        returned_peer_ids.push_back(observer_adnl_id_hex(node.adnl_id_short()));
        returned_nodes.push_back(node.clone());
      }
      valid_returned_peers = static_cast<td::uint32>(returned_nodes.size());
    }
  }
  if (!success) {
    overlay_observer_failed_queries_++;
  }

  auto target_hex = observer_adnl_id_hex(target);
  auto &member = overlay_observer_members_[target_hex];
  if (member.adnl_id.empty()) {
    member.adnl_id = target_hex;
  }
  if (member.first_request_at == 0.0) {
    member.first_request_at = request_at;
  }
  member.last_request_at = request_at;
  member.last_response_at = response_at;
  member.last_latency = response_at - request_at;
  member.requests++;
  member.last_returned_peers = returned_peers;
  if (member.max_returned_peers < returned_peers) {
    member.max_returned_peers = returned_peers;
  }
  auto overlay_hex = observer_overlay_id_hex(overlay_id);
  member.last_overlay = overlay_hex;
  auto shard_it = overlay_observer_overlay_shards_.find(overlay_id);
  auto shard_str = shard_it == overlay_observer_overlay_shards_.end() ? std::string() : shard_it->second.to_str();
  member.last_shard = shard_str;
  auto overlay_it = member.overlays.find(overlay_hex);
  if (overlay_it == member.overlays.end()) {
    member.overlays.emplace(std::move(overlay_hex), shard_str);
  } else if (overlay_it->second.empty() && !shard_str.empty()) {
    overlay_it->second = shard_str;
  }
  bool was_first_success = member.last_success_at == 0.0;
  if (success) {
    member.last_success_at = response_at;
    member.successes++;
    member.consecutive_failures = 0;
    member.last_error.clear();
    if (was_first_success) {
      queue_overlay_observer_member_queries(target, opts_.overlay_observer_.queries_per_member_, true,
                                            false /* require_address */);
    }
  } else {
    member.last_fail_at = response_at;
    member.failures++;
    member.consecutive_failures++;
    member.last_error = error;
  }
  if (success) {
    for (const auto &node : returned_nodes) {
      auto peer = node.adnl_id_short();
      auto peer_hex = observer_adnl_id_hex(peer);
      td::actor::send_closure(adnl_, &adnl::Adnl::add_peer, adnl_id_, node.adnl_id_full(), adnl::AdnlAddressList{});
      if (peer != target) {
        if (member.neighbours.insert(peer_hex).second) {
          overlay_observer_members_dirty_ = true;
        }
        if (observer_update_recent_neighbours(member.recent_neighbours, peer_hex)) {
          overlay_observer_members_dirty_ = true;
        }
      }
      bool new_member_from_response = add_overlay_observer_peer(overlay_id, peer, true, true, node.version(), node.flags());
      if (new_member_from_response) {
        new_peers++;
        new_peer_ids.push_back(peer_hex);
      }
    }
    if (!returned_nodes.empty()) {
      std::vector<tl_object_ptr<ton_api::overlay_node>> node_tls;
      node_tls.reserve(returned_nodes.size());
      for (const auto &node : returned_nodes) {
        node_tls.push_back(node.tl());
      }
      td::actor::send_closure(overlays_, &overlay::Overlays::receive_overlay_nodes, adnl_id_, overlay_id,
                              create_tl_object<ton_api::overlay_nodes>(std::move(node_tls)));
    }
  }
  overlay_observer_members_dirty_ = true;
  if (new_peers > 0) {
    dump_overlay_observer_peer_state();
  }

  if (overlay_observer_members_log_stream_) {
    overlay_observer_members_log_stream_ << std::fixed << std::setprecision(6) << "{\"event\":\"overlay_member_query\""
                                         << ",\"request_at\":" << request_at << ",\"response_at\":" << response_at
                                         << ",\"latency\":" << member.last_latency
                                         << ",\"overlay\":" << observer_json_quote(member.last_overlay)
                                         << ",\"shard\":" << observer_json_quote(member.last_shard)
                                         << ",\"target_adnl_id\":" << observer_json_quote(target_hex)
                                         << ",\"success\":" << (success ? "true" : "false")
                                         << ",\"returned_peers\":" << returned_peers
                                         << ",\"valid_returned_peers\":" << valid_returned_peers
                                         << ",\"returned_peer_ids\":";
    observer_write_json_string_array(overlay_observer_members_log_stream_, returned_peer_ids);
    overlay_observer_members_log_stream_ << ",\"new_peer_ids\":";
    observer_write_json_string_array(overlay_observer_members_log_stream_, new_peer_ids);
    overlay_observer_members_log_stream_ << std::fixed << std::setprecision(6)
                                         << ",\"new_peers\":" << new_peers
                                         << ",\"consecutive_failures\":" << member.consecutive_failures
                                         << ",\"active_queries\":" << overlay_observer_active_queries_
                                         << ",\"queued_queries\":" << overlay_observer_queue_.size();
    if (!member.last_conn_ip.empty()) {
      overlay_observer_members_log_stream_ << ",\"last_conn_ip\":" << observer_json_quote(member.last_conn_ip);
    }
    if (member.last_conn_ip_at != 0.0) {
      overlay_observer_members_log_stream_ << ",\"last_conn_ip_at\":" << member.last_conn_ip_at;
    }
    if (member.success_query_but_no_ip_at != 0.0) {
      overlay_observer_members_log_stream_ << ",\"success_query_but_no_ip_at\":"
                                           << member.success_query_but_no_ip_at;
    }
    if (!member.address_source.empty()) {
      overlay_observer_members_log_stream_ << ",\"address_source\":" << observer_json_quote(member.address_source);
    }
    if (!error.empty()) {
      overlay_observer_members_log_stream_ << ",\"error\":" << observer_json_quote(error);
    }
    overlay_observer_members_log_stream_ << "}\n";
    overlay_observer_members_log_stream_.flush();
  }

  if (success && member.addresses.empty()) {
    lookup_overlay_observer_success_address(target, response_at);
  }
  pump_overlay_observer_address_resolves();
  pump_overlay_observer_queries();
}

void FullNodeImpl::open_overlay_observer_fec_parts_log() {
  overlay_observer_fec_parts_stream_.close();
  overlay_observer_fec_parts_bytes_ = 0;
  auto stat = td::stat(overlay_observer_fec_parts_path_);
  if (stat.is_ok() && stat.ok().is_reg_ && stat.ok().size_ > 0) {
    overlay_observer_fec_parts_bytes_ = static_cast<td::uint64>(stat.ok().size_);
  }
  overlay_observer_fec_parts_stream_.open(overlay_observer_fec_parts_path_, std::ios::out | std::ios::app);
  if (!overlay_observer_fec_parts_stream_) {
    LOG(WARNING) << "failed to open FEC parts log " << overlay_observer_fec_parts_path_;
  }
}

void FullNodeImpl::rotate_overlay_observer_fec_parts_if_needed() {
  if (overlay_observer_fec_parts_bytes_ >= OVERLAY_OBSERVER_FEC_PARTS_ROTATE_BYTES) {
    rotate_overlay_observer_fec_parts_log();
  }
}

void FullNodeImpl::rotate_overlay_observer_fec_parts_log() {
  if (overlay_observer_fec_parts_stream_) {
    overlay_observer_fec_parts_stream_.flush();
    overlay_observer_fec_parts_stream_.close();
  }

  auto stat = td::stat(overlay_observer_fec_parts_path_);
  if (stat.is_error() || !stat.ok().is_reg_ || stat.ok().size_ <= 0) {
    open_overlay_observer_fec_parts_log();
    return;
  }

  auto backup_dir = overlay_observer_dir_ + "/backup";
  auto S = td::mkpath(backup_dir);
  if (S.is_error()) {
    LOG(WARNING) << "failed to create FEC parts backup dir " << backup_dir << ": " << S;
    open_overlay_observer_fec_parts_log();
    return;
  }

  std::string target;
  auto timestamp = observer_timestamp_utc();
  for (td::uint32 i = 0; i < 1000; i++) {
    target = backup_dir + "/fec-parts-" + timestamp + (i == 0 ? "" : PSTRING() << "." << i) + ".jsonl";
    if (td::stat(target).is_error()) {
      break;
    }
  }
  auto R = td::rename(overlay_observer_fec_parts_path_, target);
  if (R.is_error()) {
    LOG(WARNING) << "failed to rotate FEC parts log " << overlay_observer_fec_parts_path_ << " to " << target << ": "
                 << R;
  } else {
    LOG(WARNING) << "rotated FEC parts log to " << target;
  }

  open_overlay_observer_fec_parts_log();
  prune_overlay_observer_fec_parts_logs();
}

void FullNodeImpl::prune_overlay_observer_fec_parts_logs() {
  struct FecPartsLogFile {
    std::string path;
    td::uint64 size = 0;
    td::uint64 mtime_nsec = 0;
  };

  td::uint64 active_size = overlay_observer_fec_parts_bytes_;
  auto active_stat = td::stat(overlay_observer_fec_parts_path_);
  if (active_stat.is_ok() && active_stat.ok().is_reg_ && active_stat.ok().size_ > 0) {
    active_size = static_cast<td::uint64>(active_stat.ok().size_);
  }
  // Reserve one full active segment so retained rotated logs plus the current
  // active file cannot exceed the configured retention between rotations.
  auto reserved_active_size = std::max(active_size, OVERLAY_OBSERVER_FEC_PARTS_ROTATE_BYTES);
  td::uint64 rotated_budget = reserved_active_size >= OVERLAY_OBSERVER_FEC_PARTS_RETAIN_BYTES
                                  ? 0
                                  : OVERLAY_OBSERVER_FEC_PARTS_RETAIN_BYTES - reserved_active_size;

  auto backup_dir = overlay_observer_dir_ + "/backup";
  std::vector<FecPartsLogFile> files;
  auto S = td::WalkPath::run(backup_dir, [&](td::CSlice path, td::WalkPath::Type type) {
    if (type != td::WalkPath::Type::RegularFile) {
      return;
    }
    auto file_path = path.str();
    auto pos = file_path.find_last_of(TD_DIR_SLASH);
    auto name = pos == std::string::npos ? file_path : file_path.substr(pos + 1);
    if (!observer_has_prefix(name, "fec-parts-") || !observer_has_suffix(name, ".jsonl")) {
      return;
    }
    auto file_stat = td::stat(file_path);
    if (file_stat.is_error() || !file_stat.ok().is_reg_ || file_stat.ok().size_ <= 0) {
      return;
    }
    files.push_back(
        FecPartsLogFile{std::move(file_path), static_cast<td::uint64>(file_stat.ok().size_), file_stat.ok().mtime_nsec_});
  });
  if (S.is_error()) {
    return;
  }

  std::sort(files.begin(), files.end(), [](const FecPartsLogFile &a, const FecPartsLogFile &b) {
    if (a.mtime_nsec != b.mtime_nsec) {
      return a.mtime_nsec > b.mtime_nsec;
    }
    return a.path > b.path;
  });

  td::uint64 kept_size = 0;
  for (const auto &file : files) {
    if (kept_size + file.size <= rotated_budget) {
      kept_size += file.size;
      continue;
    }
    auto U = td::unlink(file.path);
    if (U.is_error()) {
      LOG(WARNING) << "failed to remove old FEC parts log " << file.path << ": " << U;
    } else {
      LOG(WARNING) << "removed old FEC parts log " << file.path;
    }
  }
}

void FullNodeImpl::receive_fec_broadcast_part(ShardIdFull shard, overlay::FecBroadcastPartInfo info) {
  if (!opts_.overlay_observer_.enabled_ || !overlay_observer_initialized_) {
    return;
  }
  overlay_observer_overlay_shards_[info.overlay_id] = shard;
  overlay_observer_fec_parts_received_++;
  auto now = td::Clocks::system();
  auto type = observer_fec_type_name(info.broadcast_type);
  auto direct_sender_id = observer_adnl_id_hex(info.direct_sender);
  auto origin_id = observer_adnl_id_hex(info.origin_broadcaster);
  auto source_key = observer_public_key_hash_hex(info.source_key_id);
  auto data_hash = observer_bits256_hex(info.data_hash);
  auto part_hash = observer_bits256_hex(info.part_hash);
  auto part_data_hash = observer_bits256_hex(info.part_data_hash);
  auto broadcast_hash = observer_bits256_hex(info.broadcast_hash);
  bool new_member_from_fec = add_overlay_observer_peer(info.overlay_id, info.direct_sender, true, true);

  auto &stat = overlay_observer_fec_senders_[direct_sender_id];
  bool is_new_sender = false;
  if (stat.adnl_id.empty()) {
    stat.adnl_id = direct_sender_id;
    stat.first_seen = now;
    is_new_sender = true;
  }
  stat.last_seen = now;
  stat.fec_parts++;
  stat.parts_by_type[type]++;
  stat.last_type = type;
  stat.last_origin_broadcaster_adnl_id = origin_id;
  stat.last_source_key_id = source_key;
  stat.last_data_hash = data_hash;
  stat.last_part_hash = part_hash;
  stat.last_part_data_hash = part_data_hash;
  stat.last_broadcast_hash = broadcast_hash;
  stat.last_seqno = info.seqno;
  stat.has_last_seqno = true;
  overlay_observer_fec_senders_dirty_ = true;
  if (is_new_sender) {
    dump_overlay_observer_peer_state();
    rebuild_overlay_observer_queue();
  }
  if (new_member_from_fec) {
    dump_overlay_observer_peer_state();
  }
  pump_overlay_observer_address_resolves();

  if (overlay_observer_fec_parts_stream_) {
    std::ostringstream fec_line;
    fec_line << std::fixed << std::setprecision(6) << "{\"event\":\"fec_part\""
             << ",\"ts\":" << now << ",\"overlay\":" << observer_json_quote(observer_overlay_id_hex(info.overlay_id))
             << ",\"shard\":" << observer_json_quote(shard.to_str()) << ",\"type\":" << observer_json_quote(type)
             << ",\"broadcast_type\":" << info.broadcast_type
             << ",\"direct_sender_adnl_id\":" << observer_json_quote(direct_sender_id)
             << ",\"origin_broadcaster_adnl_id\":" << observer_json_quote(origin_id)
             << ",\"source_key_id\":" << observer_json_quote(source_key) << ",\"wire_size\":" << info.wire_size
             << ",\"broadcast_data_size\":" << info.broadcast_data_size << ",\"part_size\":" << info.part_size
             << ",\"seqno\":" << info.seqno << ",\"flags\":" << info.flags << ",\"date\":" << info.date
             << ",\"signature_size\":" << info.signature_size
             << ",\"duplicate\":" << (info.duplicate ? "true" : "false");
    if (!data_hash.empty()) {
      fec_line << ",\"data_hash\":" << observer_json_quote(data_hash);
    }
    if (!part_hash.empty()) {
      fec_line << ",\"part_hash\":" << observer_json_quote(part_hash);
    }
    if (!part_data_hash.empty()) {
      fec_line << ",\"part_data_hash\":" << observer_json_quote(part_data_hash);
    }
    if (!broadcast_hash.empty()) {
      fec_line << ",\"broadcast_hash\":" << observer_json_quote(broadcast_hash);
    }
    fec_line << "}\n";
    auto line = fec_line.str();
    overlay_observer_fec_parts_stream_ << line;
    if (overlay_observer_fec_parts_stream_) {
      overlay_observer_fec_parts_bytes_ += line.size();
      rotate_overlay_observer_fec_parts_if_needed();
    }
  }
}

void FullNodeImpl::alarm() {
  if (!opts_.overlay_observer_.enabled_) {
    return;
  }
  pump_overlay_observer_address_resolves();
  pump_overlay_observer_queries();
  if (overlay_observer_flush_at_.is_in_past()) {
    if (overlay_observer_fec_senders_dirty_ || overlay_observer_members_dirty_) {
      dump_overlay_observer_peer_state();
    }
    if (overlay_observer_fec_parts_stream_) {
      overlay_observer_fec_parts_stream_.flush();
    }
    if (overlay_observer_members_log_stream_) {
      overlay_observer_members_log_stream_.flush();
    }
    overlay_observer_flush_at_ = td::Timestamp::in(30.0);
  }
  alarm_timestamp() = td::Timestamp::in(1.0);
  alarm_timestamp().relax(overlay_observer_flush_at_);
}

void FullNodeImpl::tear_down() {
  if (overlay_observer_fec_senders_dirty_ || overlay_observer_members_dirty_) {
    dump_overlay_observer_peer_state();
  }
  if (overlay_observer_fec_parts_stream_) {
    overlay_observer_fec_parts_stream_.flush();
  }
  if (overlay_observer_members_log_stream_) {
    overlay_observer_members_log_stream_.flush();
  }
}

void FullNodeImpl::start_up() {
  init_overlay_observer();
  update_shard_actor(ShardIdFull{masterchainId}, true);
  if (local_id_.is_zero()) {
    if (adnl_id_.is_zero()) {
      auto pk = ton::PrivateKey{ton::privkeys::Ed25519::random()};
      local_id_ = pk.compute_short_id();

      td::actor::send_closure(keyring_, &ton::keyring::Keyring::add_key, std::move(pk), true, [](td::Result<>) {});
    } else {
      local_id_ = adnl_id_.pubkey_hash();
    }
  }
  class Callback : public ValidatorManagerInterface::Callback {
   public:
    void initial_read_complete(BlockHandle handle) override {
      td::actor::send_closure(id_, &FullNodeImpl::initial_read_complete, handle);
    }
    void on_new_masterchain_block(td::Ref<MasterchainState> state, std::set<ShardIdFull> shards_to_monitor) override {
      td::actor::send_closure(id_, &FullNodeImpl::on_new_masterchain_block, std::move(state),
                              std::move(shards_to_monitor));
    }
    void send_ihr_message(AccountIdPrefixFull dst, td::BufferSlice data) override {
      td::actor::send_closure(id_, &FullNodeImpl::send_ihr_message, dst, std::move(data));
    }
    void send_ext_message(AccountIdPrefixFull dst, td::BufferSlice data) override {
      td::actor::send_closure(id_, &FullNodeImpl::send_ext_message, dst, std::move(data));
    }
    void send_shard_block_info(BlockIdExt block_id, CatchainSeqno cc_seqno, td::BufferSlice data) override {
      td::actor::send_closure(id_, &FullNodeImpl::send_shard_block_info, block_id, cc_seqno, std::move(data));
    }
    void send_block_candidate(BlockIdExt block_id, CatchainSeqno cc_seqno, td::uint32 validator_set_hash,
                              td::BufferSlice data, int mode) override {
      td::actor::send_closure(id_, &FullNodeImpl::send_block_candidate, block_id, cc_seqno, validator_set_hash,
                              std::move(data), mode);
    }
    void send_out_msg_queue_proof_broadcast(td::Ref<OutMsgQueueProofBroadcast> broadcast) override {
      td::actor::send_closure(id_, &FullNodeImpl::send_out_msg_queue_proof_broadcast, std::move(broadcast));
    }
    void send_broadcast(BlockBroadcast broadcast, int mode) override {
      td::actor::send_closure(id_, &FullNodeImpl::send_broadcast, std::move(broadcast), mode);
    }
    void download_block(BlockIdExt id, td::uint32 priority, td::Timestamp timeout,
                        td::Promise<ReceivedBlock> promise) override {
      td::actor::send_closure(id_, &FullNodeImpl::download_block, id, priority, timeout, std::move(promise));
    }
    void download_zero_state(BlockIdExt id, td::uint32 priority, td::Timestamp timeout,
                             td::Promise<td::BufferSlice> promise) override {
      td::actor::send_closure(id_, &FullNodeImpl::download_zero_state, id, priority, timeout, std::move(promise));
    }
    void download_persistent_state(BlockIdExt id, BlockIdExt masterchain_block_id, PersistentStateType type,
                                   td::uint32 priority, td::Timestamp timeout,
                                   td::Promise<td::BufferSlice> promise) override {
      td::actor::send_closure(id_, &FullNodeImpl::download_persistent_state, id, masterchain_block_id, type, priority,
                              timeout, std::move(promise));
    }
    void download_block_proof(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                              td::Promise<td::BufferSlice> promise) override {
      td::actor::send_closure(id_, &FullNodeImpl::download_block_proof, block_id, priority, timeout,
                              std::move(promise));
    }
    void download_block_proof_link(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                                   td::Promise<td::BufferSlice> promise) override {
      td::actor::send_closure(id_, &FullNodeImpl::download_block_proof_link, block_id, priority, timeout,
                              std::move(promise));
    }
    void get_next_key_blocks(BlockIdExt block_id, td::Timestamp timeout,
                             td::Promise<std::vector<BlockIdExt>> promise) override {
      td::actor::send_closure(id_, &FullNodeImpl::get_next_key_blocks, block_id, timeout, std::move(promise));
    }
    void download_archive(BlockSeqno masterchain_seqno, ShardIdFull shard_prefix, std::string tmp_dir,
                          td::Timestamp timeout, td::Promise<std::string> promise) override {
      td::actor::send_closure(id_, &FullNodeImpl::download_archive, masterchain_seqno, shard_prefix, std::move(tmp_dir),
                              timeout, std::move(promise));
    }
    void download_out_msg_queue_proof(ShardIdFull dst_shard, std::vector<BlockIdExt> blocks,
                                      block::ImportedMsgQueueLimits limits, td::Timestamp timeout,
                                      td::Promise<std::vector<td::Ref<OutMsgQueueProof>>> promise) override {
      td::actor::send_closure(id_, &FullNodeImpl::download_out_msg_queue_proof, dst_shard, std::move(blocks), limits,
                              timeout, std::move(promise));
    }

    void new_key_block(BlockHandle handle) override {
      td::actor::send_closure(id_, &FullNodeImpl::new_key_block, std::move(handle));
    }

    explicit Callback(td::actor::ActorId<FullNodeImpl> id) : id_(id) {
    }

   private:
    td::actor::ActorId<FullNodeImpl> id_;
  };

  td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::install_callback,
                          std::make_unique<Callback>(actor_id(this)), std::move(started_promise_));
}

void FullNodeImpl::update_private_overlays() {
  for (auto &p : custom_overlays_) {
    update_custom_overlay(p.second);
  }

  update_validator_telemetry_collector();
  if (local_keys_.empty()) {
    return;
  }
}

void FullNodeImpl::update_custom_overlay(CustomOverlayInfo &overlay) {
  auto old_actors = std::move(overlay.actors_);
  overlay.actors_.clear();
  CustomOverlayParams &params = overlay.params_;
  auto try_local_id = [&](const adnl::AdnlNodeIdShort &local_id) {
    if (std::find(params.nodes_.begin(), params.nodes_.end(), local_id) != params.nodes_.end()) {
      auto it = old_actors.find(local_id);
      if (it != old_actors.end()) {
        overlay.actors_[local_id] = std::move(it->second);
        old_actors.erase(it);
      } else {
        auto adnl_sender = (params.use_quic_ ? td::actor::ActorId<adnl::AdnlSenderEx>{quic_} : rldp2_);
        overlay.actors_[local_id] = td::actor::create_actor<FullNodeCustomOverlay>(
            "CustomOverlay", local_id, params, zero_state_file_hash_, opts_, keyring_, adnl_, adnl_sender, overlays_,
            validator_manager_, actor_id(this));
      }
    }
  };
  try_local_id(adnl_id_);
  for (const PublicKeyHash &local_key : local_keys_) {
    auto it = current_validators_.find(local_key);
    if (it != current_validators_.end()) {
      try_local_id(it->second);
    }
  }
}

void FullNodeImpl::send_block_broadcast_to_custom_overlays(const BlockBroadcast &broadcast) {
  if (custom_overlays_sent_broadcasts_.contains(broadcast.block_id)) {
    return;
  }
  custom_overlays_sent_broadcasts_.put(broadcast.block_id, {});
  for (auto &[_, private_overlay] : custom_overlays_) {
    if (private_overlay.params_.send_shard(broadcast.block_id.shard_full())) {
      for (auto &[local_id, actor] : private_overlay.actors_) {
        if (private_overlay.params_.block_senders_.contains(local_id)) {
          td::actor::send_closure(actor, &FullNodeCustomOverlay::send_broadcast, broadcast.clone());
        }
      }
    }
  }
}

void FullNodeImpl::send_block_candidate_broadcast_to_custom_overlays(const BlockIdExt &block_id, CatchainSeqno cc_seqno,
                                                                     td::uint32 validator_set_hash,
                                                                     const td::BufferSlice &data) {
  // Same cache of sent broadcasts as in send_block_broadcast_to_custom_overlays
  if (custom_overlays_sent_broadcasts_.contains(block_id)) {
    return;
  }
  custom_overlays_sent_broadcasts_.put(block_id, {});
  for (auto &[_, private_overlay] : custom_overlays_) {
    if (private_overlay.params_.send_shard(block_id.shard_full())) {
      for (auto &[local_id, actor] : private_overlay.actors_) {
        if (private_overlay.params_.block_senders_.contains(local_id)) {
          td::actor::send_closure(actor, &FullNodeCustomOverlay::send_block_candidate, block_id, cc_seqno,
                                  validator_set_hash, data.clone());
        }
      }
    }
  }
}

void FullNodeImpl::send_shard_block_info_to_custom_overlays(BlockIdExt block_id, CatchainSeqno cc_seqno,
                                                            const td::BufferSlice &data) {
  if (custom_overlays_sent_shard_block_desc_.contains(block_id)) {
    return;
  }
  custom_overlays_sent_shard_block_desc_.put(block_id, {});
  for (auto &[_, private_overlay] : custom_overlays_) {
    if (private_overlay.params_.send_shard(block_id.shard_full())) {
      for (auto &[local_id, actor] : private_overlay.actors_) {
        if (private_overlay.params_.block_senders_.contains(local_id)) {
          td::actor::send_closure(actor, &FullNodeCustomOverlay::send_shard_block_info, block_id, cc_seqno,
                                  data.clone());
        }
      }
    }
  }
}

FullNodeImpl::FullNodeImpl(PublicKeyHash local_id, adnl::AdnlNodeIdShort adnl_id, FileHash zero_state_file_hash,
                           FullNodeOptions opts, td::actor::ActorId<keyring::Keyring> keyring,
                           td::actor::ActorId<adnl::Adnl> adnl, td::actor::ActorId<rldp::Rldp> rldp,
                           td::actor::ActorId<rldp2::Rldp> rldp2, td::actor::ActorId<quic::QuicSender> quic,
                           td::actor::ActorId<dht::Dht> dht, td::actor::ActorId<overlay::Overlays> overlays,
                           td::actor::ActorId<ValidatorManagerInterface> validator_manager,
                           td::actor::ActorId<adnl::AdnlExtClient> client, std::string db_root,
                           td::Promise<td::Unit> started_promise)
    : local_id_(local_id)
    , adnl_id_(adnl_id)
    , zero_state_file_hash_(zero_state_file_hash)
    , keyring_(keyring)
    , adnl_(adnl)
    , rldp_(rldp)
    , rldp2_(rldp2)
    , quic_(quic)
    , dht_(dht)
    , overlays_(overlays)
    , validator_manager_(validator_manager)
    , client_(client)
    , db_root_(db_root)
    , started_promise_(std::move(started_promise))
    , opts_(opts)
    , limiter_(make_limiter(opts)) {
}

td::actor::ActorOwn<FullNode> FullNode::create(
    ton::PublicKeyHash local_id, adnl::AdnlNodeIdShort adnl_id, FileHash zero_state_file_hash, FullNodeOptions opts,
    td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
    td::actor::ActorId<rldp::Rldp> rldp, td::actor::ActorId<rldp2::Rldp> rldp2,
    td::actor::ActorId<quic::QuicSender> quic, td::actor::ActorId<dht::Dht> dht,
    td::actor::ActorId<overlay::Overlays> overlays, td::actor::ActorId<ValidatorManagerInterface> validator_manager,
    td::actor::ActorId<adnl::AdnlExtClient> client, std::string db_root, td::Promise<td::Unit> started_promise) {
  return td::actor::create_actor<FullNodeImpl>("fullnode", local_id, adnl_id, zero_state_file_hash, opts, keyring, adnl,
                                               rldp, rldp2, quic, dht, overlays, validator_manager, client, db_root,
                                               std::move(started_promise));
}

FullNodeConfig::FullNodeConfig(const tl_object_ptr<ton_api::engine_validator_fullNodeConfig> &obj)
    : ext_messages_broadcast_disabled_(obj->ext_messages_broadcast_disabled_) {
}

tl_object_ptr<ton_api::engine_validator_fullNodeConfig> FullNodeConfig::tl() const {
  return create_tl_object<ton_api::engine_validator_fullNodeConfig>(ext_messages_broadcast_disabled_);
}

bool CustomOverlayParams::send_shard(const ShardIdFull &shard) const {
  return sender_shards_.empty() ||
         std::any_of(sender_shards_.begin(), sender_shards_.end(),
                     [&](const ShardIdFull &our_shard) { return shard_intersects(shard, our_shard); });
}

CustomOverlayParams CustomOverlayParams::fetch(const ton_api::engine_validator_customOverlay &f) {
  CustomOverlayParams c;
  c.name_ = f.name_;
  for (const auto &node : f.nodes_) {
    c.nodes_.emplace_back(node->adnl_id_);
    if (node->msg_sender_) {
      c.msg_senders_[adnl::AdnlNodeIdShort{node->adnl_id_}] = node->msg_sender_priority_;
    }
    if (node->block_sender_) {
      c.block_senders_.emplace(node->adnl_id_);
    }
  }
  for (const auto &shard : f.sender_shards_) {
    c.sender_shards_.push_back(create_shard_id(shard));
  }
  c.skip_public_msg_send_ = f.skip_public_msg_send_;
  c.use_quic_ = f.use_quic_;
  return c;
}

decltype(FullNodeImpl::limiter_) FullNodeImpl::make_limiter(const FullNodeOptions &opts) {
  double w_size = opts.ratelimit_window_size_;
  size_t h_limit = opts.ratelimit_heavy_;
  size_t m_limit = opts.ratelimit_medium_;
  size_t g_limit = opts.ratelimit_global_;
  return std::make_shared<RateLimiter<>>(
      RateLimit{w_size, g_limit},
      std::map<int32_t, RateLimit>{{ton_api::tonNode_getArchiveSlice::ID, {w_size, h_limit}},
                                   {ton_api::tonNode_downloadPersistentStateSliceV2::ID, {w_size, h_limit}},
                                   {ton_api::tonNode_downloadZeroState::ID, {w_size, h_limit}},

                                   {ton_api::tonNode_downloadBlock::ID, {w_size, m_limit}},
                                   {ton_api::tonNode_downloadBlockFull::ID, {w_size, m_limit}},
                                   {ton_api::tonNode_downloadNextBlockFull::ID, {w_size, m_limit}},
                                   {ton_api::tonNode_downloadBlockProof::ID, {w_size, m_limit}},
                                   {ton_api::tonNode_downloadBlockProofLink::ID, {w_size, m_limit}},
                                   {ton_api::tonNode_downloadKeyBlockProof::ID, {w_size, m_limit}},
                                   {ton_api::tonNode_downloadKeyBlockProofLink::ID, {w_size, m_limit}},
                                   {ton_api::tonNode_getOutMsgQueueProof::ID, {w_size, m_limit}}});
}

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
