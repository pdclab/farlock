/*
 * Modifications Copyright (c) 2026 Copyright (C) 2026 Yuehao Hu, Jiatang Zhou,
 * Tianzheng Wang, and Keval Vora
 *
 * Copyright (c) 2024 Scalable Systems and Software Research Group, Lehigh
 * University
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#pragma once
#include <memory>
#include <unordered_set>

#include "rdma/memory_pool.h"
#include "status.h"
#include "third_party/remus/rdma/rdma_ptr.h"

using MemoryPool = rdma::MemoryPool;
using Pointer = rdma::Pointer;

namespace remus::rdma {
#define CACHELINE_SIZE 64

#define REMUS_DEBUG SPDLOG_DEBUG
#define REMUS_FATAL SPDLOG_FATAL
#define REMUS_TRACE SPDLOG_TRACE
#define REMUS_INFO SPDLOG_INFO
#define REMUS_ASSERT(check, ...)      \
    if (!(check)) [[unlikely]] {      \
        SPDLOG_CRITICAL(__VA_ARGS__); \
        std::_Exit(1);                \
    }

#if defined(__x86_64__) || defined(__i386__)
inline void cpu_relax() { asm volatile("pause\n" : : : "memory"); }
#elif defined(__aarch64__) || defined(__arm__)
inline void cpu_relax() { asm volatile("yield" ::: "memory"); }
#else
inline void cpu_relax() {}
#endif

#define UNLOCKED 0
#define LOCKED 1
#define UNUSED(x) (void)(x)

struct Peer {
    Peer(uint16_t id) : id(id) {}
    uint16_t id;
};

/// rdma_node is a per-node object that, once configured, provides all of the
/// underlying features needed by threads wishing to interact with RDMA memory.
/// Broadly, this means that it has a set of connections to other machines and a
/// set of mappings from local addresses to remote addresses.
///
class rdma_capability {
    std::shared_ptr<MemoryPool> pool_;

   public:
    explicit rdma_capability(std::shared_ptr<MemoryPool> pool) : pool_(pool) {}

    ~rdma_capability() {}

    /// Allocate some memory from the local RDMA heap
    template <typename T>
    rdma_ptr<T> Allocate(size_t size = 1) {
        UNUSED(size);
        return rdma_ptr<T>(pool_->Allocate().raw());
    }

    /// Return some memory to the local RDMA heap
    template <typename T>
    void Deallocate(rdma_ptr<T> p, size_t size = 1) {
        pool_->Free(p.raw());
    }

    /// Write to an RDMA heap
    template <typename T>
    void Write(rdma_ptr<T> ptr, const T& val, rdma_ptr<T> prealloc = nullptr) {
        pool_->Write(ptr.raw(), uint64_t(val));
    }

    /// Perform a CAS on the RDMA heap
    template <typename T>
    T CompareAndSwap(rdma_ptr<T> ptr, uint64_t expected, uint64_t swap) {
        pool_->CompareAndSwap(ptr.raw(), &expected, swap);
        return T(expected);
    }

    uint64_t FetchAndAdd(rdma_ptr<uint64_t> ptr, const uint64_t val,
                         rdma_ptr<uint64_t> prealloc = nullptr) {
        UNUSED(prealloc);
        return pool_->FetchAndAdd(ptr.raw(), val);
    }

    /// Perform a CAS on the RDMA heap that loops until it succeeds
    template <typename T>
    T AtomicSwap(rdma_ptr<T> ptr, uint64_t swap, uint64_t hint = 0) {
        return T(pool_->Exchange(ptr.raw(), swap, hint));
    }

    /// Read a fixed-sized object from the RDMA heap
    template <typename T>
    rdma_ptr<T> Read(rdma_ptr<T> ptr, rdma_ptr<T> prealloc = nullptr) {
        pool_->Read(ptr.raw(), prealloc.raw(), sizeof(T));
        return rdma_ptr<T>(prealloc.raw());
    }
};
}  // namespace remus::rdma
