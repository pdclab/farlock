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
#ifndef TICKET_LOCK_H
#define TICKET_LOCK_H
#include "common.h"

namespace lock {
class RdmaTicketLockHandler {
    struct alignas(64) RdmaTicketLock {
        uint64_t ticket_;
        uint8_t padding1_[rdma::kCacheLineSize / 2 - sizeof(uint64_t)];
        uint64_t turn_;
        uint8_t padding2_[rdma::kCacheLineSize / 2 - sizeof(uint64_t)];
    };

    static_assert(alignof(RdmaTicketLock) == rdma::kCacheLineSize,
                  "RdmaTicketLock is not cache aligned");
    static_assert(sizeof(RdmaTicketLock) == rdma::kCacheLineSize,
                  "RdmaTicketLock is not cache aligned");

    static constexpr uint64_t kTicketOffset = offsetof(RdmaTicketLock, ticket_);
    static constexpr uint64_t kTurnOffset = offsetof(RdmaTicketLock, turn_);

   public:
    RdmaTicketLockHandler() = delete;
    RdmaTicketLockHandler(std::shared_ptr<rdma::MemoryPool> pool,
                          [[maybe_unused]] const LockConfig& config)
        : pool_(pool) {
        lock_pointer_ = pool_->Allocate();
        lock_ = reinterpret_cast<RdmaTicketLock*>(lock_pointer_.address());
    }

    void Lock(const rdma::Pointer& lock) {
        uint64_t my_ticket = pool_->FetchAndAdd(lock + kTicketOffset, 1);
        while (pool_->Read(lock + kTurnOffset) != my_ticket) {
            // Spin
            CpuRelax();
        }
    }

    void Unlock(const rdma::Pointer& lock) {
        pool_->FetchAndAdd(lock + kTurnOffset, 1);
    }

    bool IsLocked(const rdma::Pointer& lock) {
        pool_->Read(lock, lock_pointer_, sizeof(RdmaTicketLock));
        return lock_->ticket_ != lock_->turn_;
    }

    ~RdmaTicketLockHandler() {}

   private:
    std::shared_ptr<rdma::MemoryPool> pool_;
    rdma::Pointer lock_pointer_;
    RdmaTicketLock* lock_;
};
}  // namespace lock
#endif  // TICKET_LOCK_H