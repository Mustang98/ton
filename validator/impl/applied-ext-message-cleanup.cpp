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
*/

#include "applied-ext-message-cleanup.hpp"
#include "external-message.hpp"
#include "ton/ton-io.hpp"

namespace ton::validator {

void AppliedExtMessageCleanupActor::got_block_data(BlockIdExt block_id, td::Result<td::Ref<BlockData>> block) {
  if (block.is_error()) {
    LOG(WARNING) << "failed to load block data for applied external cleanup of block " << block_id << " : "
                 << block.move_as_error();
    return;
  }
  cleanup_applied_block(BlockHandle{}, block.move_as_ok());
}

void AppliedExtMessageCleanupActor::cleanup_applied_block(BlockHandle handle, td::Ref<BlockData> block) {
  if (block.is_null()) {
    if (!handle) {
      return;
    }
    auto block_id = handle->id();
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), block_id](td::Result<td::Ref<BlockData>> R) mutable {
      td::actor::send_closure(SelfId, &AppliedExtMessageCleanupActor::got_block_data, block_id, std::move(R));
    });
    td::actor::send_closure(manager_, &ValidatorManager::get_block_data_from_db, handle, std::move(P));
    return;
  }

  auto hashes = get_applied_external_messages_hashes(block);
  if (hashes.is_error()) {
    LOG(WARNING) << "failed to cleanup applied externals for block "
                 << (block.is_null() ? "(null)" : block->block_id().to_str()) << " : " << hashes.move_as_error();
    return;
  }
  auto values = hashes.move_as_ok();
  if (values.empty()) {
    return;
  }
  LOG(INFO) << "cleanup applied externals for block " << block->block_id() << " : normalized_hashes=" << values.size();
  td::actor::send_closure(ext_message_pool_, &ExtMessagePool::erase_external_messages, std::move(values));
}

}  // namespace ton::validator
