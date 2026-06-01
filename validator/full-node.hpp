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
#pragma once

#include "full-node-shard.h"
#include "full-node.h"
//#include "ton-node-slave.h"
#include <deque>
#include <fstream>
#include <map>
#include <queue>
#include <set>
#include <token-manager.h>

#include "interfaces/proof.h"
#include "interfaces/shard.h"
#include "td/utils/LRUCache.h"

#include "full-node-custom-overlays.hpp"
#include "full-node-fast-sync-overlays.hpp"
#include "rate-limiter.h"

namespace ton {

namespace validator {

namespace fullnode {

class FullNodeImpl : public FullNode {
 public:
  void update_dht_node(td::actor::ActorId<dht::Dht> dht) override;

  void add_permanent_key(PublicKeyHash key, td::Promise<td::Unit> promise) override;
  void del_permanent_key(PublicKeyHash key, td::Promise<td::Unit> promise) override;
  void add_collator_adnl_id(adnl::AdnlNodeIdShort id) override;
  void del_collator_adnl_id(adnl::AdnlNodeIdShort id) override;

  void sign_shard_overlay_certificate(ShardIdFull shard_id, PublicKeyHash signed_key, td::uint32 expiry_at,
                                      td::uint32 max_size, td::Promise<td::BufferSlice> promise) override;
  void import_shard_overlay_certificate(ShardIdFull shard_id, PublicKeyHash signed_key,
                                        std::shared_ptr<ton::overlay::Certificate> cert,
                                        td::Promise<td::Unit> promise) override;

  void update_adnl_id(adnl::AdnlNodeIdShort adnl_id, td::Promise<td::Unit> promise) override;
  void set_config(FullNodeConfig config) override;

  void add_custom_overlay(CustomOverlayParams params, td::Promise<td::Unit> promise) override;
  void del_custom_overlay(std::string name, td::Promise<td::Unit> promise) override;

  void on_new_masterchain_block(td::Ref<MasterchainState> state, std::set<ShardIdFull> shards_to_monitor);

  void sync_completed();

  void initial_read_complete(BlockHandle top_block);
  void send_ihr_message(AccountIdPrefixFull dst, td::BufferSlice data);
  void send_ext_message(AccountIdPrefixFull dst, td::BufferSlice data);
  void send_shard_block_info(BlockIdExt block_id, CatchainSeqno cc_seqno, td::BufferSlice data);
  void send_block_candidate(BlockIdExt block_id, CatchainSeqno cc_seqno, td::uint32 validator_set_hash,
                            td::BufferSlice data, int mode);
  void send_broadcast(BlockBroadcast broadcast, int mode);
  void send_out_msg_queue_proof_broadcast(td::Ref<OutMsgQueueProofBroadcast> broadcats);
  void download_block(BlockIdExt id, td::uint32 priority, td::Timestamp timeout, td::Promise<ReceivedBlock> promise);
  void download_zero_state(BlockIdExt id, td::uint32 priority, td::Timestamp timeout,
                           td::Promise<td::BufferSlice> promise);
  void download_persistent_state(BlockIdExt id, BlockIdExt masterchain_block_id, PersistentStateType type,
                                 td::uint32 priority, td::Timestamp timeout, td::Promise<td::BufferSlice> promise);
  void download_block_proof(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                            td::Promise<td::BufferSlice> promise);
  void download_block_proof_link(BlockIdExt block_id, td::uint32 priority, td::Timestamp timeout,
                                 td::Promise<td::BufferSlice> promise);
  void get_next_key_blocks(BlockIdExt block_id, td::Timestamp timeout, td::Promise<std::vector<BlockIdExt>> promise);
  void download_archive(BlockSeqno masterchain_seqno, ShardIdFull shard_prefix, std::string tmp_dir,
                        td::Timestamp timeout, td::Promise<std::string> promise);
  void download_out_msg_queue_proof(ShardIdFull dst_shard, std::vector<BlockIdExt> blocks,
                                    block::ImportedMsgQueueLimits limits, td::Timestamp timeout,
                                    td::Promise<std::vector<td::Ref<OutMsgQueueProof>>> promise);

  void got_key_block_config(td::Ref<ConfigHolder> config);
  void new_key_block(BlockHandle handle);

  void process_block_broadcast(BlockBroadcast broadcast, bool signatures_checked = false) override;
  void process_block_candidate_broadcast(BlockIdExt block_id, CatchainSeqno cc_seqno, td::uint32 validator_set_hash,
                                         td::BufferSlice data) override;
  void process_shard_block_info_broadcast(BlockIdExt block_id, CatchainSeqno cc_seqno, td::BufferSlice data) override;
  void get_out_msg_queue_query_token(td::Promise<std::unique_ptr<ActionToken>> promise) override;

  void set_validator_telemetry_filename(std::string value) override;
  void receive_fec_broadcast_part(ShardIdFull shard, overlay::FecBroadcastPartInfo info) override;

  void import_fast_sync_member_certificate(adnl::AdnlNodeIdShort local_id,
                                           overlay::OverlayMemberCertificate cert) override {
    VLOG(FULL_NODE_DEBUG) << "Importing fast sync overlay certificate for " << local_id << " issued by "
                          << cert.issued_by().compute_short_id() << " expires in "
                          << (double)cert.expire_at() - td::Clocks::system();
    fast_sync_overlays_.add_member_certificate(local_id, std::move(cert));
  }

  void start_up() override;
  void alarm() override;
  void tear_down() override;

  FullNodeImpl(PublicKeyHash local_id, adnl::AdnlNodeIdShort adnl_id, FileHash zero_state_file_hash,
               FullNodeOptions opts, td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
               td::actor::ActorId<rldp::Rldp> rldp, td::actor::ActorId<rldp2::Rldp> rldp2,
               td::actor::ActorId<quic::QuicSender> quic, td::actor::ActorId<dht::Dht> dht,
               td::actor::ActorId<overlay::Overlays> overlays,
               td::actor::ActorId<ValidatorManagerInterface> validator_manager,
               td::actor::ActorId<adnl::AdnlExtClient> client, std::string db_root,
               td::Promise<td::Unit> started_promise);

 private:
  struct ShardInfo {
    td::actor::ActorOwn<FullNodeShard> actor;
    bool active = false;
    td::Timestamp delete_at = td::Timestamp::never();
    bool overlay_observer_seeded = false;
  };
  using OverlayObserverTarget = std::pair<overlay::OverlayIdShort, adnl::AdnlNodeIdShort>;
  struct OverlayObserverAddress {
    std::string type;
    std::string ip;
    td::int32 port = 0;
  };
  struct OverlayObserverMember;

  void update_shard_actor(ShardIdFull shard, bool active);
  void init_overlay_observer();
  void load_overlay_observer_peers();
  bool load_overlay_observer_peer_state();
  void dump_overlay_observer_peer_state();
  void seed_overlay_observer_peers(ShardIdFull shard, ShardInfo &info);
  bool add_overlay_observer_peer(overlay::OverlayIdShort overlay_id, adnl::AdnlNodeIdShort peer, bool enqueue,
                                 bool dirty, td::int32 version = 0, td::uint32 flags = 0);
  bool overlay_observer_can_query_peer(overlay::OverlayIdShort overlay_id, adnl::AdnlNodeIdShort peer,
                                       bool require_address = true) const;
  bool overlay_observer_peer_is_recent(const std::string &peer_hex, double now) const;
  bool overlay_observer_peer_has_persisted_activity(const std::string &peer_hex, const OverlayObserverMember &member,
                                                    double now) const;
  td::uint64 prune_overlay_observer_peer_state(double now);
  bool queue_overlay_observer_member_queries(adnl::AdnlNodeIdShort peer, td::uint32 count, bool front,
                                             bool require_address);
  bool queue_overlay_observer_probe_queries(adnl::AdnlNodeIdShort peer, bool front);
  bool choose_overlay_observer_oldest_target(OverlayObserverTarget &target) const;
  void queue_overlay_observer_target(overlay::OverlayIdShort overlay_id, adnl::AdnlNodeIdShort peer,
                                     td::uint32 count = 1, bool front = false, bool require_address = true);
  bool choose_overlay_observer_oldest_address_target(adnl::AdnlNodeIdShort &target) const;
  void pump_overlay_observer_address_resolves();
  void finish_overlay_observer_address_resolve(adnl::AdnlNodeIdShort peer, td::Result<dht::DhtValue> result);
  void lookup_overlay_observer_success_address(adnl::AdnlNodeIdShort peer, double ts);
  void finish_overlay_observer_success_peer_table_address(adnl::AdnlNodeIdShort peer, double ts,
                                                          td::Result<adnl::Adnl::PeerAddrLists> result);
  void finish_overlay_observer_success_connection_address(adnl::AdnlNodeIdShort peer, double ts,
                                                          std::string peer_table_error,
                                                          td::Result<td::string> result);
  std::vector<OverlayObserverAddress> extract_overlay_observer_addresses(
      const adnl::AdnlAddressList &addr_list) const;
  bool apply_overlay_observer_addresses(adnl::AdnlNodeIdShort peer, adnl::AdnlNodeIdFull full_id,
                                        adnl::AdnlAddressList addr_list, std::string source, std::string *error);
  td::Result<OverlayObserverAddress> parse_overlay_observer_connection_endpoint(td::Slice endpoint) const;
  bool apply_overlay_observer_connection_endpoint(adnl::AdnlNodeIdShort peer, td::Slice endpoint, double ts);
  void log_overlay_observer_address_resolve(td::Slice source, double ts, td::Slice peer_hex, bool success,
                                            size_t address_count, size_t active_resolves, td::Slice error = {});
  void rebuild_overlay_observer_queue();
  void pump_overlay_observer_queries();
  void send_overlay_observer_query(overlay::OverlayIdShort overlay_id, adnl::AdnlNodeIdShort target);
  void send_signed_overlay_observer_query(overlay::OverlayIdShort overlay_id, adnl::AdnlNodeIdShort target,
                                          overlay::OverlayNode self_node, double request_at);
  void finish_overlay_observer_query(overlay::OverlayIdShort overlay_id, adnl::AdnlNodeIdShort target,
                                     double request_at, td::Result<td::BufferSlice> result);
  void finish_overlay_observer_query_error(overlay::OverlayIdShort overlay_id, adnl::AdnlNodeIdShort target,
                                           double request_at, std::string error);
  void write_overlay_observer_members_log_line(std::string line);
  void open_overlay_observer_members_log();
  void rotate_overlay_observer_members_log_if_needed();
  void rotate_overlay_observer_members_log();
  void prune_overlay_observer_members_logs();
  void open_overlay_observer_fec_parts_log();
  void rotate_overlay_observer_fec_parts_if_needed();
  void rotate_overlay_observer_fec_parts_log();
  void prune_overlay_observer_fec_parts_logs();

  PublicKeyHash local_id_;
  adnl::AdnlNodeIdShort adnl_id_;
  FileHash zero_state_file_hash_;

  td::actor::ActorId<FullNodeShard> get_shard(AccountIdPrefixFull dst);
  td::actor::ActorId<FullNodeShard> get_shard(ShardIdFull shard, bool historical = false);
  std::map<ShardIdFull, ShardInfo> shards_;
  int wc_monitor_min_split_ = 0;

  struct OverlayObserverFecSender {
    std::string adnl_id;
    double first_seen = 0.0;
    double last_seen = 0.0;
    td::uint64 fec_parts = 0;
    std::map<std::string, td::uint64> parts_by_type;
    std::string last_type;
    std::string last_origin_broadcaster_adnl_id;
    std::string last_source_key_id;
    std::string last_data_hash;
    std::string last_part_hash;
    std::string last_part_data_hash;
    std::string last_broadcast_hash;
    td::int32 last_seqno = 0;
    bool has_last_seqno = false;
  };
  struct OverlayObserverMember {
    std::string adnl_id;
    td::int32 version = 0;
    td::uint32 flags = 0;
    double first_request_at = 0.0;
    double last_request_at = 0.0;
    double last_response_at = 0.0;
    double last_success_at = 0.0;
    double last_fail_at = 0.0;
    double last_latency = 0.0;
    td::uint64 requests = 0;
    td::uint64 successes = 0;
    td::uint64 failures = 0;
    td::uint32 consecutive_failures = 0;
    td::uint32 last_returned_peers = 0;
    td::uint32 max_returned_peers = 0;
    std::string last_overlay;
    std::string last_shard;
    std::map<std::string, std::string> overlays;
    std::string last_error;
    std::set<std::string> neighbours;
    std::vector<std::string> recent_neighbours;
    std::vector<OverlayObserverAddress> addresses;
    double address_updated_at = 0.0;
    std::string address_source;
    double last_dht_success_at = 0.0;
    double last_dht_fail_at = 0.0;
    td::uint64 dht_successes = 0;
    td::uint64 dht_failures = 0;
    std::string last_dht_error;
    std::string last_conn_ip;
    double last_conn_ip_at = 0.0;
    double success_query_but_no_ip_at = 0.0;
  };
  struct OverlayObserverQuery {
    overlay::OverlayIdShort overlay_id;
    adnl::AdnlNodeIdShort target;
    double request_at = 0.0;
  };

  bool overlay_observer_initialized_ = false;
  std::string overlay_observer_dir_;
  std::string overlay_observer_peers_path_;
  std::string overlay_observer_fec_parts_path_;
  std::string overlay_observer_members_log_path_;
  std::map<overlay::OverlayIdShort, ShardIdFull> overlay_observer_overlay_shards_;
  std::map<overlay::OverlayIdShort, std::vector<adnl::AdnlNodeIdShort>> overlay_observer_initial_peers_;
  std::map<overlay::OverlayIdShort, std::set<adnl::AdnlNodeIdShort>> overlay_observer_known_peers_;
  std::deque<OverlayObserverTarget> overlay_observer_queue_;
  std::set<std::string> overlay_observer_active_query_targets_;
  td::uint32 overlay_observer_active_queries_ = 0;
  std::set<std::string> overlay_observer_active_address_resolves_;
  td::uint64 overlay_observer_queries_ = 0;
  td::uint64 overlay_observer_failed_queries_ = 0;
  td::uint64 overlay_observer_fec_parts_received_ = 0;
  td::uint64 overlay_observer_fec_parts_bytes_ = 0;
  td::uint64 overlay_observer_members_log_bytes_ = 0;
  std::ofstream overlay_observer_fec_parts_stream_;
  std::ofstream overlay_observer_members_log_stream_;
  std::map<std::string, OverlayObserverFecSender> overlay_observer_fec_senders_;
  std::map<std::string, OverlayObserverMember> overlay_observer_members_;
  bool overlay_observer_fec_senders_dirty_ = false;
  bool overlay_observer_members_dirty_ = false;
  td::uint64 overlay_observer_last_dump_peer_count_ = 0;
  bool overlay_observer_peer_state_dump_blocked_ = false;
  td::Timestamp overlay_observer_flush_at_ = td::Timestamp::never();

  td::actor::ActorId<keyring::Keyring> keyring_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  td::actor::ActorId<rldp::Rldp> rldp_;
  td::actor::ActorId<rldp2::Rldp> rldp2_;
  td::actor::ActorId<quic::QuicSender> quic_;
  td::actor::ActorId<dht::Dht> dht_;
  td::actor::ActorId<overlay::Overlays> overlays_;
  td::actor::ActorId<ValidatorManagerInterface> validator_manager_;
  td::actor::ActorId<adnl::AdnlExtClient> client_;

  std::string db_root_;

  PublicKeyHash sign_cert_by_;
  std::vector<PublicKeyHash> all_validators_;
  std::map<PublicKeyHash, adnl::AdnlNodeIdShort> current_validators_;

  std::set<PublicKeyHash> local_keys_;
  std::map<adnl::AdnlNodeIdShort, int> local_collator_nodes_;

  td::Promise<td::Unit> started_promise_;
  FullNodeOptions opts_;

  FullNodeFastSyncOverlays fast_sync_overlays_;

  struct CustomOverlayInfo {
    CustomOverlayParams params_;
    std::map<adnl::AdnlNodeIdShort, td::actor::ActorOwn<FullNodeCustomOverlay>> actors_;  // our local id -> actor
  };
  std::map<std::string, CustomOverlayInfo> custom_overlays_;
  td::LRUCache<BlockIdExt, td::Unit> custom_overlays_sent_broadcasts_{256};
  td::LRUCache<BlockIdExt, td::Unit> custom_overlays_sent_shard_block_desc_{256};

  void update_private_overlays();
  void update_custom_overlay(CustomOverlayInfo& overlay);
  void send_block_broadcast_to_custom_overlays(const BlockBroadcast& broadcast);
  void send_block_candidate_broadcast_to_custom_overlays(const BlockIdExt& block_id, CatchainSeqno cc_seqno,
                                                         td::uint32 validator_set_hash, const td::BufferSlice& data);
  void send_shard_block_info_to_custom_overlays(BlockIdExt block_id, CatchainSeqno cc_seqno,
                                                const td::BufferSlice& data);

  std::string validator_telemetry_filename_;
  PublicKeyHash validator_telemetry_collector_key_ = PublicKeyHash::zero();

  void update_validator_telemetry_collector();

  td::actor::ActorOwn<TokenManager> out_msg_queue_query_token_manager_ =
      td::actor::create_actor<TokenManager>("tokens", /* max_tokens = */ 1);

  std::shared_ptr<RateLimiter<>> limiter_;

  decltype(limiter_) make_limiter(const FullNodeOptions& opts);
};

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
