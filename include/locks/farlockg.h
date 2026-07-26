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
#ifndef LOCKS_FARLOCKG_H
#define LOCKS_FARLOCKG_H

#include "common.h"

namespace lock {
class FARLockGHandler {
    // Just use a single cacheline for every structure,
    // since RDMA operations are relatively slow and
    // we confirmed the performance is not affected by this.
    struct alignas(rdma::kCacheLineSize) RemoteDescriptor {
        rdma::Pointer next_;
        uint64_t ticket_valid_;
        uint64_t batch_ticket_;
        uint64_t budget_;
        uint64_t granted_;
        uint64_t releasable_;
        uint8_t padding_[rdma::kCacheLineSize - 6 * sizeof(uint64_t)];
    };
    static_assert(alignof(RemoteDescriptor) == rdma::kCacheLineSize,
                  "RemoteDescriptor is not cache aligned");
    static_assert(sizeof(RemoteDescriptor) == rdma::kCacheLineSize,
                  "RemoteDescriptor is not cache aligned");
    struct alignas(rdma::kCacheLineSize) LocalDescriptor {
        LocalDescriptor* next_;
        uint64_t ticket_valid_;
        uint64_t batch_ticket_;
        uint64_t budget_;
        uint64_t granted_;
        uint64_t releasable_;
        uint8_t padding_[16];
    };
    static_assert(alignof(LocalDescriptor) == rdma::kCacheLineSize,
                  "LocalDescriptor is not cache aligned");
    static_assert(sizeof(LocalDescriptor) == rdma::kCacheLineSize,
                  "LocalDescriptor is not cache aligned");
    struct alignas(rdma::kCacheLineSize) FARLockG {
        RemoteDescriptor* remote_tail_;
        LocalDescriptor* local_tail_;
        uint64_t remote_flag_;
        uint64_t victim_;
        uint64_t local_flag_;
        volatile uint64_t next_ticket_;
        volatile uint64_t current_ticket_;
        uint8_t padding_[8];
    };
    static_assert(alignof(FARLockG) == rdma::kCacheLineSize,
                  "FARLockG is not cache aligned");
    static_assert(sizeof(FARLockG) == rdma::kCacheLineSize,
                  "FARLockG is not cache aligned");
    // Offset of the fields in the RemoteDescriptor
    static constexpr uint64_t kRemoteNextOffset =
        offsetof(RemoteDescriptor, next_);
    static constexpr uint64_t kTicketValidOffset =
        offsetof(RemoteDescriptor, ticket_valid_);
    static constexpr uint64_t kRemoteBatchTicketOffset =
        offsetof(RemoteDescriptor, batch_ticket_);
    static constexpr uint64_t kRemoteBudgetOffset =
        offsetof(RemoteDescriptor, budget_);
    static constexpr uint64_t kRemoteGrantedOffset =
        offsetof(RemoteDescriptor, granted_);
    static constexpr uint64_t kRemoteReleasableOffset =
        offsetof(RemoteDescriptor, releasable_);
    // Offset of the fields in the FARLockG
    static constexpr uint64_t kRemoteTailOffset =
        offsetof(FARLockG, remote_tail_);
    static constexpr uint64_t kLocalTailOffset =
        offsetof(FARLockG, local_tail_);
    static constexpr uint64_t kRemoteFlagOffset =
        offsetof(FARLockG, remote_flag_);
    static constexpr uint64_t kVictimOffset = offsetof(FARLockG, victim_);
    static constexpr uint64_t kNextTicketOffset =
        offsetof(FARLockG, next_ticket_);
    static constexpr uint64_t kCurrentTicketOffset =
        offsetof(FARLockG, current_ticket_);

    static constexpr uint64_t kInvalidTicket = 0;
    static constexpr uint64_t kValidTicket = 1;
    static constexpr uint64_t kBatchTicket = 2;

    void LockPetersonsLocal(FARLockG* lock) {
        StoreVal(lock->local_flag_, kTrue);
        StoreVal(lock->victim_, kLocalVictim);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        while (LoadVal(lock->remote_flag_) == kTrue &&
               LoadVal(lock->victim_) == kLocalVictim) {
            CpuRelax();
        }
        std::atomic_thread_fence(std::memory_order_acquire);
    }

    void UnlockPetersonsLocal(FARLockG* lock) {
        std::atomic_thread_fence(std::memory_order_release);
        StoreVal(lock->local_flag_, kFalse);
    }

    void LockPetersonsRemote(const rdma::Pointer& lock) {
        lock_buffer_->remote_flag_ = kTrue;
        lock_buffer_->victim_ = kRemoteVictim;
        pool_->Write(lock + kRemoteFlagOffset,
                     lock_buffer_pointer_ + kRemoteFlagOffset, 16);
        while (true) {
            pool_->Read(lock + kVictimOffset,
                        lock_buffer_pointer_ + kVictimOffset, 16);
            if (lock_buffer_->local_flag_ != kTrue ||
                lock_buffer_->victim_ != kRemoteVictim) {
                return;
            }
            CpuRelax();
        }
        std::atomic_thread_fence(std::memory_order_acquire);
    }

    void UnlockPetersonsRemote(const rdma::Pointer& lock) {
        std::atomic_thread_fence(std::memory_order_release);
        pool_->Write(lock + kRemoteFlagOffset, kFalse);
    }

    void LockLocal(FARLockG* lock) {
        uint64_t ticket;
        local_descriptor_.next_ = nullptr;
        local_descriptor_.ticket_valid_ = kFalse;
        local_descriptor_.budget_ = -1;
        local_descriptor_.granted_ = kFalse;
        local_descriptor_.releasable_ = kFalse;
        bool ticket_from_prev = false;
        auto prev = __atomic_exchange_n(&lock->local_tail_, &local_descriptor_,
                                        __ATOMIC_ACQ_REL);
        if (prev) {
            __atomic_store_n(&prev->next_, &local_descriptor_,
                             __ATOMIC_SEQ_CST);
            ticket = __atomic_load_n(&prev->ticket_valid_, __ATOMIC_SEQ_CST);
            __atomic_store_n(&prev->releasable_, kTrue, __ATOMIC_RELEASE);
            if (ticket != kTrue) {
                while (__atomic_load_n(&local_descriptor_.budget_,
                                       __ATOMIC_SEQ_CST) == -1) {
                    CpuRelax();
                }
                if (local_descriptor_.budget_ != kInitLocalBudget_) {
                    ticket = local_descriptor_.batch_ticket_;
                    ticket_from_prev = true;
                    goto got_ticket;
                }
            }
        }
        local_descriptor_.budget_ = kInitLocalBudget_;
        LockPetersonsLocal(lock);
        ticket = __atomic_fetch_add(&lock->next_ticket_, 1, __ATOMIC_SEQ_CST);
    got_ticket:
        if (local_descriptor_.next_) {
            if (local_descriptor_.budget_ == 1) {
                UnlockPetersonsLocal(lock);
                local_descriptor_.ticket_valid_ = kTrue;
                __atomic_store_n(&local_descriptor_.next_->budget_,
                                 kInitLocalBudget_, __ATOMIC_SEQ_CST);
            } else {
                __atomic_store_n(&local_descriptor_.next_->batch_ticket_,
                                 ticket, __ATOMIC_SEQ_CST);
                __atomic_store_n(&local_descriptor_.next_->budget_,
                                 local_descriptor_.budget_ - 1,
                                 __ATOMIC_SEQ_CST);
            }
        } else {
            UnlockPetersonsLocal(lock);
            __atomic_store_n(&local_descriptor_.ticket_valid_, kTrue,
                             __ATOMIC_SEQ_CST);
            if (__atomic_load_n(&local_descriptor_.next_, __ATOMIC_SEQ_CST)) {
                __atomic_store_n(&local_descriptor_.next_->budget_,
                                 kInitLocalBudget_, __ATOMIC_RELEASE);
            }
        }
        if (prev) {
            while (__atomic_load_n(&local_descriptor_.granted_,
                                   __ATOMIC_ACQUIRE) != kTrue) {
                CpuRelax();
            }
        }
        if (ticket_from_prev) {
            return;
        }
        while (ticket != LoadVal(lock->current_ticket_)) {
            CpuRelax();
        }
        local_descriptor_.batch_ticket_ = ticket;
    }

    void UnlockLocal(FARLockG* lock) {
        if (local_descriptor_.ticket_valid_ == kTrue) {
            __atomic_store_n(&lock->current_ticket_,
                             local_descriptor_.batch_ticket_ + 1,
                             __ATOMIC_SEQ_CST);
        }
        if (__atomic_load_n(&local_descriptor_.next_, __ATOMIC_ACQUIRE) ==
            nullptr) {
            uint64_t addr = reinterpret_cast<uint64_t>(&local_descriptor_);
            if (__atomic_compare_exchange_n(&lock->local_tail_, &addr, nullptr,
                                            false, __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE)) {
                return;
            }
        }
        while (__atomic_load_n(&local_descriptor_.releasable_,
                               __ATOMIC_ACQUIRE) != kTrue);

        __atomic_store_n(&local_descriptor_.next_->granted_, kTrue,
                         __ATOMIC_RELEASE);
    }

    void LockRemote(const rdma::Pointer& lock) {
        uint64_t ticket;
        remote_descriptor_->next_ = nullptr;
        remote_descriptor_->ticket_valid_ = kFalse;
        remote_descriptor_->budget_ = -1;
        remote_descriptor_->granted_ = kFalse;
        remote_descriptor_->releasable_ = kFalse;
        bool ticket_from_prev = false;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        rdma::Pointer prev = pool_->Exchange(lock + kRemoteTailOffset,
                                             remote_descriptor_pointer_.raw());

        if (prev != nullptr) {
            pool_->Write(prev + kRemoteNextOffset,
                         remote_descriptor_pointer_.raw());
            std::atomic_thread_fence(std::memory_order_seq_cst);
            ticket = pool_->Read(prev + kTicketValidOffset);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            pool_->Write(prev + kRemoteReleasableOffset, kTrue);
            if (ticket != kTrue) {
                while (LoadVal(remote_descriptor_->budget_) == -1) {
                    CpuRelax();
                }
                if (remote_descriptor_->budget_ != kInitRemoteBudget_) {
                    ticket = remote_descriptor_->batch_ticket_;
                    ticket_from_prev = true;
                    goto got_ticket;
                }
            }
        }
        remote_descriptor_->budget_ = kInitRemoteBudget_;
        LockPetersonsRemote(lock);
        ticket = pool_->FetchAndAdd(lock + kNextTicketOffset, 1);
    got_ticket:
        if (LoadVal(remote_descriptor_->next_)) {
            assert(remote_descriptor_->budget_ > 0);
            if (remote_descriptor_->budget_ == 1) {
                UnlockPetersonsRemote(lock);
                std::atomic_thread_fence(std::memory_order_seq_cst);
                remote_descriptor_->ticket_valid_ = kTrue;
                pool_->Write(remote_descriptor_->next_ + kRemoteBudgetOffset,
                             kInitRemoteBudget_);
            } else {
                remote_descriptor_buffer_->batch_ticket_ = ticket;
                remote_descriptor_buffer_->budget_ =
                    remote_descriptor_->budget_ - 1;
                pool_->Write(
                    remote_descriptor_->next_ + kRemoteBatchTicketOffset,
                    remote_descriptor_buffer_pointer_ +
                        kRemoteBatchTicketOffset,
                    16);
            }
        } else {
            UnlockPetersonsRemote(lock);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            StoreVal(remote_descriptor_->ticket_valid_, kTrue);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            if (LoadVal(remote_descriptor_->next_)) {
                pool_->Write(remote_descriptor_->next_ + kRemoteBudgetOffset,
                             kInitRemoteBudget_);
            }
        }
        if (prev != nullptr) {
            while (LoadVal(remote_descriptor_->granted_) != kTrue) {
                CpuRelax();
            }
        }
        if (ticket_from_prev) {
            return;
        }
        while (ticket != pool_->Read(lock + kCurrentTicketOffset)) {
            CpuRelax();
        }
        remote_descriptor_->batch_ticket_ = ticket;
    }

    void UnlockRemote(const rdma::Pointer& lock) {
        if (remote_descriptor_->ticket_valid_ == kTrue) {
            pool_->Write(lock + kCurrentTicketOffset,
                         remote_descriptor_->batch_ticket_ + 1);
        }
        if (LoadVal(remote_descriptor_->next_) == 0) {
            if (pool_->CompareAndSwap(lock + kRemoteTailOffset,
                                      remote_descriptor_pointer_.raw(), 0)) {
                return;
            }
        }
        while (LoadVal(remote_descriptor_->releasable_) != kTrue);

        pool_->Write(remote_descriptor_->next_ + kRemoteGrantedOffset, kTrue);
    }

   public:
    FARLockGHandler() = delete;
    FARLockGHandler(std::shared_ptr<rdma::MemoryPool> pool,
                    [[maybe_unused]] const LockConfig& config)
        : pool_(pool),
          kInitLocalBudget_(config.local_budget_),
          kInitRemoteBudget_(config.remote_budget_) {
        lock_buffer_pointer_ = pool_->Allocate();
        lock_buffer_ =
            reinterpret_cast<FARLockG*>(lock_buffer_pointer_.address());
        remote_descriptor_pointer_ = pool_->Allocate();
        remote_descriptor_ = reinterpret_cast<RemoteDescriptor*>(
            remote_descriptor_pointer_.address());
        remote_descriptor_buffer_pointer_ = pool_->Allocate();
        remote_descriptor_buffer_ = reinterpret_cast<RemoteDescriptor*>(
            remote_descriptor_buffer_pointer_.address());
    }

    void Lock(const rdma::Pointer& lock) {
        if (pool_->IsLocal(lock)) {
            LockLocal(reinterpret_cast<FARLockG*>(lock.address()));
        } else {
            LockRemote(lock);
        }
        std::atomic_thread_fence(std::memory_order_acquire);
    }

    void Unlock(const rdma::Pointer& lock) {
        std::atomic_thread_fence(std::memory_order_release);
        if (pool_->IsLocal(lock)) {
            UnlockLocal(reinterpret_cast<FARLockG*>(lock.address()));
        } else {
            UnlockRemote(lock);
        }
    }

    bool IsLockedLocal(FARLockG* lock) {
        return reinterpret_cast<void*>(lock->remote_tail_) !=
               reinterpret_cast<void*>(lock->local_tail_);
    }
    bool IsLockedRemote(const rdma::Pointer& lock) {
        pool_->Read(lock, lock_buffer_pointer_, sizeof(FARLockG));
        return reinterpret_cast<void*>(lock_buffer_->remote_tail_) !=
               reinterpret_cast<void*>(lock_buffer_->local_tail_);
    }

    bool IsLocked(const rdma::Pointer& lock) {
        if (pool_->IsLocal(lock)) {
            return IsLockedLocal(reinterpret_cast<FARLockG*>(lock.address()));
        } else {
            return IsLockedRemote(lock);
        }
    }

   private:
    std::shared_ptr<rdma::MemoryPool> pool_;
    LocalDescriptor local_descriptor_;
    RemoteDescriptor* remote_descriptor_;
    RemoteDescriptor* remote_descriptor_buffer_;
    rdma::Pointer remote_descriptor_pointer_;
    rdma::Pointer remote_descriptor_buffer_pointer_;
    FARLockG* lock_buffer_;
    rdma::Pointer lock_buffer_pointer_;
    const int64_t kInitLocalBudget_;
    const int64_t kInitRemoteBudget_;
};
}  // namespace lock
#endif  // LOCKS_FARLOCKG_H