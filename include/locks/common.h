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
#ifndef RDMA_LOCK_H
#define RDMA_LOCK_H

#include "rdma/memory_pool.h"

namespace lock {

#if defined(__x86_64__) || defined(__i386__)
// follows previous work's implementation. Otherwise, it may cause RDMA errors
inline void CpuRelax() { asm volatile("pause\n" : : : "memory"); }
#elif defined(__aarch64__) || defined(__arm__)
inline void CpuRelax() { asm volatile("yield" ::: "memory"); }
#else
inline void CpuRelax() {}
#endif

#define LoadVal(v) (*(volatile uint64_t*)&(v))
#define StoreVal(v, val) (*(volatile uint64_t*)&(v) = (val))

static constexpr uint64_t kTrue = 1;
static constexpr uint64_t kFalse = 0;
static constexpr uint64_t kLocked = 1;
static constexpr uint64_t kUnlocked = 0;

static constexpr uint64_t kLocalVictim = 1;
static constexpr uint64_t kRemoteVictim = 2;

struct LockConfig {
    uint32_t local_budget_;
    uint32_t remote_budget_;
};

class RdmaLockHandler {
   public:
    RdmaLockHandler(std::shared_ptr<rdma::MemoryPool> pool,
                    [[maybe_unused]] const LockConfig& config)
        : pool_(pool) {}
    virtual void Lock(const rdma::Pointer& lock) = 0;
    virtual void Unlock(const rdma::Pointer& lock) = 0;
    virtual bool IsLocked(const rdma::Pointer& lock) = 0;
    virtual ~RdmaLockHandler() = default;

   protected:
    const std::shared_ptr<rdma::MemoryPool> pool_;
};
};  // namespace lock
#endif  // RDMA_LOCK_H