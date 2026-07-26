/*
 * Copyright (C) 2026 Yuehao Hu, Jiatang Zhou, Tianzheng Wang, and Keval Vora
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * If you use this code or parts of it in any work (including commercial,
 * open-source, academic, or non-academic projects), please cite:
 * Yuehao Hu, Jiatang Zhou, Tianzheng Wang, and Keval Vora, "FARLock:
 * Asymmetric RDMA Locking Made Fair," in Proceedings of the 20th USENIX
 * Symposium on Operating Systems Design and Implementation (OSDI '26), 2026.
 */
#pragma once
#include <memory>

#include "locks/a_lock.h"
namespace lock {
class A_LockHandler {
   public:
    A_LockHandler(std::shared_ptr<rdma::MemoryPool> pool,
                  [[maybe_unused]] const LockConfig& config)
        : pool_(pool) {
        uint16_t node_id = pool_->GetNodeId();
        std::unordered_set<int> local_clients;
        local_clients.insert(node_id);
        std::shared_ptr<remus::rdma::rdma_capability> cap =
            std::make_shared<remus::rdma::rdma_capability>(pool_);
        lock_ = std::make_unique<ALockHandle>(node_id, cap, local_clients,
                                              config.local_budget_,
                                              config.remote_budget_);
        lock_->Init();
    }
    void Lock(const rdma::Pointer& lock) {
        lock_->Lock(rdma_ptr<ALock>(lock.raw()));
    }
    void Unlock(const rdma::Pointer& lock) {
        lock_->Unlock(rdma_ptr<ALock>(lock.raw()));
    }
    ~A_LockHandler() {}

   protected:
    const std::shared_ptr<rdma::MemoryPool> pool_;
    std::unique_ptr<ALockHandle> lock_;
};
};  // namespace lock
