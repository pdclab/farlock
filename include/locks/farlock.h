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
#ifndef LOCKS_FARLOCK_H
#define LOCKS_FARLOCK_H

#include "common.h"

namespace lock {
class FARLockHandler {
    // Just use a single cacheline for every structure,
    // since RDMA operations are relatively slow and
    // we confirmed the performance is not affected by this.
    struct alignas(rdma::kCacheLineSize) RemoteDescriptor {
        rdma::Pointer next_;
        uint64_t ticket_;
        uint64_t to_get_ticket_;
        uint64_t granted_;
        uint64_t releasable_;
        uint8_t padding[rdma::kCacheLineSize - 5 * sizeof(uint64_t)];
    };
    static_assert(alignof(RemoteDescriptor) == rdma::kCacheLineSize,
                  "RemoteDescriptor is not cache aligned");
    static_assert(sizeof(RemoteDescriptor) == rdma::kCacheLineSize,
                  "RemoteDescriptor is not cache aligned");
    struct alignas(rdma::kCacheLineSize) LocalDescriptor {
        LocalDescriptor* next_;
        uint64_t ticket_;
        uint64_t to_get_ticket_;
        uint64_t granted_;
        uint64_t releasable_;
        uint8_t padding[rdma::kCacheLineSize - 5 * sizeof(uint64_t)];
    };
    static_assert(alignof(LocalDescriptor) == rdma::kCacheLineSize,
                  "LocalDescriptor is not cache aligned");
    static_assert(sizeof(LocalDescriptor) == rdma::kCacheLineSize,
                  "LocalDescriptor is not cache aligned");
    struct alignas(rdma::kCacheLineSize) FARLock {
        RemoteDescriptor* remote_tail_;
        LocalDescriptor* local_tail_;
        uint64_t remote_flag_;
        uint64_t victim_;
        uint64_t local_flag_;
        volatile uint64_t next_ticket_;
        volatile uint64_t current_ticket_;
        uint8_t padding7_[rdma::kCacheLineSize - 7 * sizeof(uint64_t)];
    };
    static_assert(alignof(FARLock) == rdma::kCacheLineSize,
                  "FARLock is not cache aligned");
    static_assert(sizeof(FARLock) == rdma::kCacheLineSize,
                  "FARLock is not cache aligned");
    // Offset of the fields in the RemoteDescriptor
    static constexpr uint64_t kRemoteNextOffset =
        offsetof(RemoteDescriptor, next_);
    static constexpr uint64_t kRemoteTicketOffset =
        offsetof(RemoteDescriptor, ticket_);
    static constexpr uint64_t kRemoteToGetTicketOffset =
        offsetof(RemoteDescriptor, to_get_ticket_);
    static constexpr uint64_t kRemoteGrantedOffset =
        offsetof(RemoteDescriptor, granted_);
    static constexpr uint64_t kRemoteReleasableOffset =
        offsetof(RemoteDescriptor, releasable_);
    // Offset of the fields in the FARLock
    static constexpr uint64_t kRemoteTailOffset =
        offsetof(FARLock, remote_tail_);
    static constexpr uint64_t kLocalTailOffset = offsetof(FARLock, local_tail_);
    static constexpr uint64_t kRemoteFlagOffset =
        offsetof(FARLock, remote_flag_);
    static constexpr uint64_t kVictimOffset = offsetof(FARLock, victim_);
    static constexpr uint64_t kNextTicketOffset =
        offsetof(FARLock, next_ticket_);
    static constexpr uint64_t kCurrentTicketOffset =
        offsetof(FARLock, current_ticket_);

    static constexpr uint64_t kInvalidTicket = -1;

    void LockPetersonsLocal(FARLock* lock) {
        StoreVal(lock->local_flag_, kTrue);
        StoreVal(lock->victim_, kLocalVictim);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        while (LoadVal(lock->remote_flag_) == kTrue &&
               LoadVal(lock->victim_) == kLocalVictim) {
            CpuRelax();
        }
    }

    void UnlockPetersonsLocal(FARLock* lock) {
        StoreVal(lock->local_flag_, kFalse);
    }

    void LockPetersonsRemote(const rdma::Pointer& lock) {
        lock_buffer_->remote_flag_ = kTrue;
        lock_buffer_->victim_ = kRemoteVictim;
        pool_->Write(lock + kRemoteFlagOffset,
                     lock_buffer_pointer_ + kRemoteFlagOffset,
                     2 * sizeof(uint64_t));
        while (true) {
            pool_->Read(lock + kVictimOffset,
                        lock_buffer_pointer_ + kVictimOffset,
                        2 * sizeof(uint64_t));
            if (lock_buffer_->local_flag_ != kTrue ||
                lock_buffer_->victim_ != kRemoteVictim) {
                return;
            }
            CpuRelax();
        }
    }

    void UnlockPetersonsRemote(const rdma::Pointer& lock) {
        pool_->Write(lock + kRemoteFlagOffset, kFalse);
    }

    void WaitToGetTicketLocal() {
        while (__atomic_load_n(&local_descriptor_.to_get_ticket_,
                               __ATOMIC_ACQUIRE) != kTrue) {
            CpuRelax();
        }
    }

    void WaitToGetTicketRemote() {
        while (LoadVal(remote_descriptor_->to_get_ticket_) != kTrue) {
            CpuRelax();
        }
    }

    void AwaitPredecessorLocal() {
        while (__atomic_load_n(&local_descriptor_.granted_, __ATOMIC_ACQUIRE) !=
               kTrue) {
            CpuRelax();
        }
    }

    void AwaitPredecessorRemote() {
        while (LoadVal(remote_descriptor_->granted_) != kTrue) {
            CpuRelax();
        }
    }

    void AwaitTicketLocal(FARLock* lock) {
        while (local_descriptor_.ticket_ != LoadVal(lock->current_ticket_)) {
            CpuRelax();
        }
    }

    void AwaitTicketRemote(const rdma::Pointer lock) {
        while (remote_descriptor_->ticket_ !=
               pool_->Read(lock + kCurrentTicketOffset)) {
            CpuRelax();
        }
    }

    void LockLocal(FARLock* lock) {
        uint64_t ticket;
        local_descriptor_.next_ = nullptr;
        local_descriptor_.ticket_ = kInvalidTicket;
        local_descriptor_.to_get_ticket_ = kFalse;
        local_descriptor_.granted_ = kFalse;
        local_descriptor_.releasable_ = kFalse;
        auto prev = __atomic_exchange_n(&lock->local_tail_, &local_descriptor_,
                                        __ATOMIC_ACQ_REL);
        if (prev) {
            __atomic_store_n(&prev->next_, &local_descriptor_,
                             __ATOMIC_SEQ_CST);
            ticket = __atomic_load_n(&prev->ticket_, __ATOMIC_SEQ_CST);
            __atomic_store_n(&prev->releasable_, kTrue, __ATOMIC_RELEASE);
            if (ticket == kInvalidTicket) {
                WaitToGetTicketLocal();
            }
        }

        LockPetersonsLocal(lock);
        ticket = __atomic_fetch_add(&lock->next_ticket_, 1, __ATOMIC_RELEASE);
        UnlockPetersonsLocal(lock);
        __atomic_store_n(&local_descriptor_.ticket_, ticket, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&local_descriptor_.next_, __ATOMIC_SEQ_CST)) {
            __atomic_store_n(&local_descriptor_.next_->to_get_ticket_, kTrue,
                             __ATOMIC_RELEASE);
        }
        if (prev) {
            AwaitPredecessorLocal();
        }
        AwaitTicketLocal(lock);
    }

    void UnlockLocal(FARLock* lock) {
        __atomic_fetch_add(&lock->current_ticket_, 1, __ATOMIC_RELEASE);
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
                               __ATOMIC_ACQUIRE) != kTrue) {
            CpuRelax();
        }
        __atomic_store_n(&local_descriptor_.next_->granted_, kTrue,
                         __ATOMIC_RELEASE);
    }

    void LockRemote(const rdma::Pointer& lock) {
        uint64_t ticket;
        remote_descriptor_->next_ = nullptr;
        remote_descriptor_->ticket_ = kInvalidTicket;
        remote_descriptor_->to_get_ticket_ = kFalse;
        remote_descriptor_->granted_ = kFalse;
        remote_descriptor_->releasable_ = kFalse;
        std::atomic_thread_fence(std::memory_order_acq_rel);
        rdma::Pointer prev = pool_->Exchange(lock + kRemoteTailOffset,
                                             remote_descriptor_pointer_.raw());
        if (prev != nullptr) {
            pool_->Write(prev + kRemoteNextOffset,
                         remote_descriptor_pointer_.raw());
            std::atomic_thread_fence(std::memory_order_seq_cst);
            ticket = pool_->Read(prev + kRemoteTicketOffset);
            pool_->Write(prev + kRemoteReleasableOffset, kTrue);
            if (ticket == kInvalidTicket) {
                WaitToGetTicketRemote();
            }
        }

        LockPetersonsRemote(lock);
        ticket = pool_->FetchAndAdd(lock + kNextTicketOffset, 1);
        UnlockPetersonsRemote(lock);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        StoreVal(remote_descriptor_->ticket_, ticket);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (LoadVal(remote_descriptor_->next_)) {
            pool_->Write(remote_descriptor_->next_ + kRemoteToGetTicketOffset,
                         kTrue);
        }
        if (prev != nullptr) {
            AwaitPredecessorRemote();
        }
        AwaitTicketRemote(lock);
    }

    void UnlockRemote(const rdma::Pointer& lock) {
        pool_->Write(lock + kCurrentTicketOffset,
                     remote_descriptor_->ticket_ + 1);
        if (LoadVal(remote_descriptor_->next_) == 0) {
            if (pool_->CompareAndSwap(lock + kRemoteTailOffset,
                                      remote_descriptor_pointer_.raw(), 0)) {
                return;
            }
        }
        while (LoadVal(remote_descriptor_->releasable_) != kTrue) {
            CpuRelax();
        }
        pool_->Write(remote_descriptor_->next_ + kRemoteGrantedOffset, kTrue);
    }

   public:
    FARLockHandler() = delete;
    FARLockHandler(std::shared_ptr<rdma::MemoryPool> pool,
                   [[maybe_unused]] const LockConfig& config)
        : pool_(pool) {
        lock_buffer_pointer_ = pool_->Allocate();
        lock_buffer_ =
            reinterpret_cast<FARLock*>(lock_buffer_pointer_.address());
        remote_descriptor_pointer_ = pool_->Allocate();
        remote_descriptor_ = reinterpret_cast<RemoteDescriptor*>(
            remote_descriptor_pointer_.address());
    }

    void Lock(const rdma::Pointer& lock) {
        if (pool_->IsLocal(lock)) {
            LockLocal(reinterpret_cast<FARLock*>(lock.address()));
        } else {
            LockRemote(lock);
        }
        std::atomic_thread_fence(std::memory_order_acquire);
    }

    void Unlock(const rdma::Pointer& lock) {
        std::atomic_thread_fence(std::memory_order_release);
        if (pool_->IsLocal(lock)) {
            UnlockLocal(reinterpret_cast<FARLock*>(lock.address()));
        } else {
            UnlockRemote(lock);
        }
    }

    bool IsLockedLocal(FARLock* lock) {
        return reinterpret_cast<void*>(lock->remote_tail_) !=
               reinterpret_cast<void*>(lock->local_tail_);
    }
    bool IsLockedRemote(const rdma::Pointer& lock) {
        pool_->Read(lock, lock_buffer_pointer_, sizeof(FARLock));
        return reinterpret_cast<void*>(lock_buffer_->remote_tail_) !=
               reinterpret_cast<void*>(lock_buffer_->local_tail_);
    }

    bool IsLocked(const rdma::Pointer& lock) {
        if (pool_->IsLocal(lock)) {
            return IsLockedLocal(reinterpret_cast<FARLock*>(lock.address()));
        } else {
            return IsLockedRemote(lock);
        }
    }

   private:
    std::shared_ptr<rdma::MemoryPool> pool_;
    LocalDescriptor local_descriptor_;
    RemoteDescriptor* remote_descriptor_;
    rdma::Pointer remote_descriptor_pointer_;
    FARLock* lock_buffer_;
    rdma::Pointer lock_buffer_pointer_;
};
}  // namespace lock
#endif  // LOCKS_FARLOCK_H