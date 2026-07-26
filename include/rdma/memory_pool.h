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
#ifndef RDMA_MEMORY_POOL_H
#define RDMA_MEMORY_POOL_H
// #pragma once
#include <vector>

#include "pointer.h"

namespace rdma {

class alignas(kCacheLineSize) MemoryPool {
   public:
    MemoryPool(internal::FreeChunk free_list, const size_t chunk_size,
               const uint16_t node_id, const uint32_t lkey,
               const std::vector<std::shared_ptr<TargetContext>>& target_map,
               uint64_t buffer_count = 58368)
        : free_list_(free_list),
          chunk_size_(chunk_size),
          node_id_(node_id),
          lkey_(lkey),
          target_map_(target_map),
          buffer_count_(buffer_count),
          buffer_index_(0) {
        assert(chunk_size >= internal::kMinChunkSize);
        assert(chunk_size <= internal::kMaxChunkSize);
        local_buffer_pointers_.resize(buffer_count_);
        uint64_t buffers_per_chunk = chunk_size / sizeof(uint64_t);
        uint64_t buffer_chunks = buffer_count_ / buffers_per_chunk;
        if (buffer_count_ % buffers_per_chunk != 0) {
            buffer_chunks++;
        }
        int i = 0;
        while (i < buffer_count_) {
            auto ptr = Allocate();
            if (!ptr) {
                SPDLOG_FATAL("MemoryPool: not enough memory for local buffers");
            }
            for (size_t j = 0; j < buffers_per_chunk && i < buffer_count_;
                 j++) {
                local_buffer_pointers_[i++] = ptr + j * sizeof(uint64_t);
            }
        }
    }

    Pointer Allocate() {
        if (free_list_.next == nullptr) {
            return nullptr;
        }
        void* ptr = free_list_.next;
        free_list_.next = free_list_.next->next;
        return Pointer(node_id_, ptr);
    }

    void Free(Pointer& ptr) {
        // check if the pointer belongs to this node
        assert(IsLocal(ptr));
        // add the chunk to the free list
        auto* chunk = reinterpret_cast<internal::FreeChunk*>(ptr.address());
        chunk->next = free_list_.next;
        free_list_.next = chunk;
    }

    void Read(const Pointer& target_ptr, const Pointer& local_ptr, size_t size,
              bool fence = true) {
        assert(IsLocal(local_ptr));
        assert(size <= chunk_size_);
        assert(target_ptr.id() < target_map_.size());
        auto& target = target_map_[target_ptr.id()];
        // prepare the read request
        ibv_sge sge = {0};
        sge.addr = reinterpret_cast<uint64_t>(local_ptr.address());
        sge.length = size;
        sge.lkey = lkey_;

        ibv_send_wr wr = {0};
        wr.wr_id = request_id_++;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        wr.opcode = IBV_WR_RDMA_READ;
        wr.send_flags = IBV_SEND_SIGNALED;
        if (fence) {
            wr.send_flags |= IBV_SEND_FENCE;
        }
        wr.wr.rdma.remote_addr =
            reinterpret_cast<uint64_t>(target_ptr.address());
        wr.wr.rdma.rkey = target->rkey_;
        ibv_send_wr* bad_wr;
        // post the read request
        int rv;
        if (rv = ibv_post_send(target->qp_, &wr, &bad_wr)) {
            SPDLOG_FATAL("Failed to post write request: {} {}", rv,
                         internal::StrError(rv));
        }
        // wait for the completion
        struct ibv_wc wc;
        int num_comp;
        do {
            num_comp = ibv_poll_cq(target->send_cq_, 1, &wc);
        } while (num_comp == 0);

        if (num_comp < 0) {
            SPDLOG_FATAL("Failed to poll CQ: {}", strerror(errno));
        }
        /* verify the completion status */
        if (wc.status != IBV_WC_SUCCESS) {
            SPDLOG_FATAL("Failed status {} ({}) for wr_id {}",
                         ibv_wc_status_str(wc.status), (int)wc.status,
                         (int)wc.wr_id);
        }
    }

    uint64_t Read(const Pointer& target_ptr, bool fence = true) {
        assert(sizeof(uint64_t) <= chunk_size_);
        assert(target_ptr.id() < target_map_.size());
        uint64_t bindex = buffer_index_++;
        buffer_index_ %= buffer_count_;
        auto bptr = local_buffer_pointers_[bindex];
        uint64_t* local = reinterpret_cast<uint64_t*>(bptr.address());
        Read(target_ptr, bptr, sizeof(uint64_t), fence);
        return *local;
    }

    void Write(const Pointer& target_ptr, const Pointer& local_ptr, size_t size,
               bool fence = true, bool signaled = true) {
        assert(IsLocal(local_ptr));
        assert(size <= chunk_size_);
        assert(target_ptr.id() < target_map_.size());
        auto& target = target_map_[target_ptr.id()];
        // prepare the write request
        ibv_sge sge = {0};
        sge.addr = reinterpret_cast<uint64_t>(local_ptr.address());
        sge.length = size;
        sge.lkey = lkey_;

        ibv_send_wr wr = {0};
        wr.wr_id = request_id_++;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        wr.opcode = IBV_WR_RDMA_WRITE;
        if (fence) {
            wr.send_flags |= IBV_SEND_FENCE;
        }
        if (signaled) {
            wr.send_flags |= IBV_SEND_SIGNALED;
        }
        wr.wr.rdma.remote_addr =
            reinterpret_cast<uint64_t>(target_ptr.address());
        wr.wr.rdma.rkey = target->rkey_;
        // post the write request
        ibv_send_wr* bad_wr;
        int rv;
        int retry_count = 0;
    retry:
        if (rv = ibv_post_send(target->qp_, &wr, &bad_wr)) {
            if (rv == ENOMEM) {
                if (retry_count++ < 1000000) {
                    if (!signaled) {
                        signaled = true;
                        wr.send_flags |= IBV_SEND_SIGNALED;
                    }
                    goto retry;
                }
            }
            SPDLOG_FATAL("Failed to post write request: {} {}", rv,
                         internal::StrError(rv));
        }
        if (signaled) {
            // wait for the completion
            struct ibv_wc wc;
            int num_comp;
            do {
                num_comp = ibv_poll_cq(target->send_cq_, 1, &wc);
            } while (num_comp == 0);

            if (num_comp < 0) {
                SPDLOG_FATAL("Failed to poll CQ: {}", strerror(errno));
            }
            /* verify the completion status */
            if (wc.status != IBV_WC_SUCCESS) {
                SPDLOG_FATAL("Failed status {} ({}) for wr_id {}",
                             ibv_wc_status_str(wc.status), (int)wc.status,
                             (int)wc.wr_id);
            }
        }
    }

    [[gnu::noinline]] void Write(const Pointer& target_ptr,
                                 const uint64_t value, bool fence = true,
                                 bool signaled = true) {
        assert(sizeof(uint64_t) <= chunk_size_);
        assert(target_ptr.id() < target_map_.size());
        uint64_t bindex = buffer_index_++;
        buffer_index_ %= buffer_count_;
        auto bptr = local_buffer_pointers_[bindex];
        uint64_t* local = reinterpret_cast<uint64_t*>(bptr.address());
        *local = value;
        Write(target_ptr, bptr, sizeof(uint64_t), fence, signaled);
    }

    [[gnu::noinline]] bool CompareAndSwap(const Pointer& target_ptr,
                                          const uint64_t expected,
                                          const uint64_t desired,
                                          bool fence = true) {
        assert(sizeof(uint64_t) <= chunk_size_);
        assert(target_ptr.id() < target_map_.size());
        auto& target = target_map_[target_ptr.id()];
        uint64_t bindex = buffer_index_++;
        buffer_index_ %= buffer_count_;
        uint64_t* prev = reinterpret_cast<uint64_t*>(
            local_buffer_pointers_[bindex].address());
        // prepare the compare-and-swap request
        ibv_sge sge = {0};
        sge.addr = reinterpret_cast<uint64_t>(prev);
        sge.length = sizeof(uint64_t);
        sge.lkey = lkey_;

        ibv_send_wr wr = {0};
        wr.wr_id = request_id_++;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        wr.opcode = IBV_WR_ATOMIC_CMP_AND_SWP;
        wr.send_flags = IBV_SEND_SIGNALED;
        if (fence) {
            wr.send_flags |= IBV_SEND_FENCE;
        }
        wr.wr.atomic.remote_addr =
            reinterpret_cast<uint64_t>(target_ptr.address());
        wr.wr.atomic.rkey = target->rkey_;
        wr.wr.atomic.compare_add = expected;
        wr.wr.atomic.swap = desired;
        // post the compare-and-swap request
        ibv_send_wr* bad_wr;
        int rv;
        if (rv = ibv_post_send(target->qp_, &wr, &bad_wr)) {
            SPDLOG_FATAL("Failed to post write request: {} {}", rv,
                         internal::StrError(rv));
        }
        // wait for the completion
        ibv_wc wc;
        int num_comp;
        do {
            num_comp = ibv_poll_cq(target->send_cq_, 1, &wc);
        } while (num_comp == 0);

        if (num_comp < 0) {
            SPDLOG_FATAL("Failed to poll CQ: {}", strerror(errno));
        }
        /* verify the completion status */
        if (wc.status != IBV_WC_SUCCESS) {
            SPDLOG_FATAL("Failed status {} ({}) for wr_id {}",
                         ibv_wc_status_str(wc.status), (int)wc.status,
                         (int)wc.wr_id);
        }
        return *prev == expected;
    }

    bool CompareAndSwap(const Pointer& target_ptr, uint64_t* expected,
                        const uint64_t desired, bool fence = true) {
        assert(sizeof(uint64_t) <= chunk_size_);
        assert(target_ptr.id() < target_map_.size());
        auto& target = target_map_[target_ptr.id()];
        uint64_t bindex = buffer_index_++;
        buffer_index_ %= buffer_count_;
        uint64_t* prev = reinterpret_cast<uint64_t*>(
            local_buffer_pointers_[bindex].address());
        // prepare the compare-and-swap request
        ibv_sge sge = {0};
        sge.addr = reinterpret_cast<uint64_t>(prev);
        sge.length = sizeof(uint64_t);
        sge.lkey = lkey_;

        ibv_send_wr wr = {0};
        wr.wr_id = request_id_++;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        wr.opcode = IBV_WR_ATOMIC_CMP_AND_SWP;
        wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_FENCE;
        wr.wr.atomic.remote_addr =
            reinterpret_cast<uint64_t>(target_ptr.address());
        wr.wr.atomic.rkey = target->rkey_;
        wr.wr.atomic.compare_add = *expected;
        wr.wr.atomic.swap = desired;
        // post the compare-and-swap request
        ibv_send_wr* bad_wr;
        int rv;
        if (rv = ibv_post_send(target->qp_, &wr, &bad_wr)) {
            SPDLOG_FATAL("Failed to post write request: {} {}", rv,
                         internal::StrError(rv));
        }
        // wait for the completion
        ibv_wc wc;
        int num_comp;
        do {
            num_comp = ibv_poll_cq(target->send_cq_, 1, &wc);
        } while (num_comp == 0);

        if (num_comp < 0) {
            SPDLOG_FATAL("Failed to poll CQ: {}", strerror(errno));
        }
        /* verify the completion status */
        if (wc.status != IBV_WC_SUCCESS) {
            SPDLOG_FATAL("Failed status {} ({}) for wr_id {}",
                         ibv_wc_status_str(wc.status), (int)wc.status,
                         (int)wc.wr_id);
        }
        bool ret = (*prev == *expected);
        *expected = *prev;
        return ret;
    }

    uint64_t FetchAndAdd(const Pointer& target_ptr, const uint64_t add,
                         bool fence = true) {
        assert(sizeof(uint64_t) <= chunk_size_);
        assert(target_ptr.id() < target_map_.size());
        auto& target = target_map_[target_ptr.id()];
        uint64_t bindex = buffer_index_++;
        buffer_index_ %= buffer_count_;
        uint64_t* prev = reinterpret_cast<uint64_t*>(
            local_buffer_pointers_[bindex].address());
        // prepare the fetch-and-add request
        ibv_sge sge = {0};
        sge.addr = reinterpret_cast<uint64_t>(prev);
        sge.length = sizeof(uint64_t);
        sge.lkey = lkey_;

        ibv_send_wr wr = {0};
        wr.wr_id = request_id_++;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        wr.opcode = IBV_WR_ATOMIC_FETCH_AND_ADD;
        wr.send_flags = IBV_SEND_SIGNALED;
        if (fence) {
            wr.send_flags |= IBV_SEND_FENCE;
        }
        wr.wr.atomic.remote_addr =
            reinterpret_cast<uint64_t>(target_ptr.address());
        wr.wr.atomic.rkey = target->rkey_;
        wr.wr.atomic.compare_add = add;
        // post the fetch-and-add request
        ibv_send_wr* bad_wr;
        int rv;
        if (rv = ibv_post_send(target->qp_, &wr, &bad_wr)) {
            SPDLOG_FATAL("Failed to post write request: {} {}", rv,
                         internal::StrError(rv));
        }
        // wait for the completion
        ibv_wc wc;
        int num_comp;
        do {
            num_comp = ibv_poll_cq(target->send_cq_, 1, &wc);
        } while (num_comp == 0);

        if (num_comp < 0) {
            SPDLOG_FATAL("Failed to poll CQ: {}", strerror(errno));
        }
        /* verify the completion status */
        if (wc.status != IBV_WC_SUCCESS) {
            SPDLOG_FATAL("Failed status {} ({}) for wr_id {}",
                         ibv_wc_status_str(wc.status), (int)wc.status,
                         (int)wc.wr_id);
        }
        return *prev;
    }

    [[gnu::noinline]] uint64_t Exchange(const Pointer& target_ptr,
                                        const uint64_t desired,
                                        uint64_t hint = 0) {
        assert(sizeof(uint64_t) <= chunk_size_);
        assert(target_ptr.id() < target_map_.size());
        auto& target = target_map_[target_ptr.id()];
        uint64_t bindex = buffer_index_++;
        buffer_index_ %= buffer_count_;
        uint64_t* prev = reinterpret_cast<uint64_t*>(
            local_buffer_pointers_[bindex].address());
        // repeatedly perform CAS until success
        ibv_sge sge = {0};
        sge.addr = reinterpret_cast<uint64_t>(prev);
        sge.length = sizeof(uint64_t);
        sge.lkey = lkey_;
        ibv_send_wr wr = {0};
        wr.wr_id = request_id_++;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        wr.opcode = IBV_WR_ATOMIC_CMP_AND_SWP;
        wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_FENCE;
        wr.wr.atomic.remote_addr =
            reinterpret_cast<uint64_t>(target_ptr.address());
        wr.wr.atomic.rkey = target->rkey_;
        wr.wr.atomic.compare_add = hint;
        wr.wr.atomic.swap = desired;
        ibv_send_wr* bad_wr;
        int rv;
        while (true) {
            if (rv = ibv_post_send(target->qp_, &wr, &bad_wr)) {
                SPDLOG_FATAL("Failed to post write request: {} {}", rv,
                             internal::StrError(rv));
            }

            struct ibv_wc wc;
            int num_comp;
            do {
                num_comp = ibv_poll_cq(target->send_cq_, 1, &wc);
            } while (num_comp == 0);

            if (num_comp < 0) {
                SPDLOG_FATAL("Failed to poll CQ: {}", strerror(errno));
            }
            /* verify the completion status */
            if (wc.status != IBV_WC_SUCCESS) {
                SPDLOG_FATAL("Failed status {} ({}) for wr_id {}",
                             ibv_wc_status_str(wc.status), (int)wc.status,
                             (int)wc.wr_id);
            }
            if (*prev == wr.wr.atomic.compare_add) {
                break;
            }
            wr.wr.atomic.compare_add = *prev;
        }
        return *prev;
    }

    void Send(const uint16_t node_id, const Pointer& local_ptr, size_t size) {
        assert(IsLocal(local_ptr));
        assert(size <= chunk_size_);
        assert(node_id < target_map_.size());
        auto& target = target_map_[node_id];
        // prepare the send request
        ibv_sge sge = {0};
        sge.addr = reinterpret_cast<uint64_t>(local_ptr.address());
        sge.length = size;
        sge.lkey = lkey_;

        ibv_send_wr wr = {0};
        wr.wr_id = request_id_++;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        wr.opcode = IBV_WR_SEND;
        wr.send_flags = IBV_SEND_SIGNALED;
        // post the send request
        ibv_send_wr* bad_wr;
        int rv;
        if (rv = ibv_post_send(target->qp_, &wr, &bad_wr)) {
            SPDLOG_FATAL("Failed to post write request: {} {}", rv,
                         internal::StrError(rv));
        }
        // wait for the completion
        ibv_wc wc;
        int num_comp;
        do {
            num_comp = ibv_poll_cq(target->send_cq_, 1, &wc);
        } while (num_comp == 0);

        if (num_comp < 0) {
            SPDLOG_FATAL("Failed to poll CQ: {}", strerror(errno));
        }
        /* verify the completion status */
        if (wc.status != IBV_WC_SUCCESS) {
            SPDLOG_FATAL("Failed status {} ({}) for wr_id {}",
                         ibv_wc_status_str(wc.status), (int)wc.status,
                         (int)wc.wr_id);
        }
    }

    void Send(const uint16_t node_id, const uint64_t value) {
        assert(sizeof(uint64_t) <= chunk_size_);
        assert(node_id < target_map_.size());
        uint64_t bindex = buffer_index_++;
        buffer_index_ %= buffer_count_;
        auto bptr = local_buffer_pointers_[bindex];
        uint64_t* local = reinterpret_cast<uint64_t*>(bptr.address());
        *local = value;
        Send(node_id, bptr, sizeof(uint64_t));
    }

    void Receive(const uint16_t node_id, const Pointer& local_ptr,
                 size_t size) {
        assert(IsLocal(local_ptr));
        assert(size <= chunk_size_);
        assert(node_id < target_map_.size());
        auto& target = target_map_[node_id];
        // prepare the receive request
        ibv_sge sge = {0};
        sge.addr = reinterpret_cast<uint64_t>(local_ptr.address());
        sge.length = size;
        sge.lkey = lkey_;

        ibv_recv_wr wr = {0};
        wr.wr_id = request_id_++;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        // post the receive request
        ibv_recv_wr* bad_wr;
        if (ibv_post_recv(target->qp_, &wr, &bad_wr)) {
            SPDLOG_FATAL("Failed to post receive request: {}", strerror(errno));
        }
        // wait for the completion
        ibv_wc wc;
        int num_comp;
        do {
            num_comp = ibv_poll_cq(target->recv_cq_, 1, &wc);
        } while (num_comp == 0);

        if (num_comp < 0) {
            SPDLOG_FATAL("Failed to poll CQ: {}", strerror(errno));
        }
        /* verify the completion status */
        if (wc.status != IBV_WC_SUCCESS) {
            SPDLOG_FATAL("Failed status {} ({}) for wr_id {}",
                         ibv_wc_status_str(wc.status), (int)wc.status,
                         (int)wc.wr_id);
        }
    }

    uint64_t Receive(const uint16_t node_id) {
        assert(sizeof(uint64_t) <= chunk_size_);
        assert(node_id < target_map_.size());
        uint64_t bindex = buffer_index_++;
        buffer_index_ %= buffer_count_;
        auto bptr = local_buffer_pointers_[bindex];
        uint64_t* local = reinterpret_cast<uint64_t*>(bptr.address());
        Receive(node_id, bptr, sizeof(uint64_t));
        return *local;
    }

    ~MemoryPool() {}

    inline bool IsLocal(const Pointer& ptr) const {
        return ptr.id() == node_id_;
    }

    uint16_t GetNodeId() const { return node_id_; }

   private:
    internal::FreeChunk free_list_;
    std::vector<Pointer> local_buffer_pointers_;
    uint64_t request_id_ = 0;
    const size_t chunk_size_;
    const uint16_t node_id_;
    const uint32_t lkey_;
    const std::vector<std::shared_ptr<TargetContext>> target_map_;
    const uint64_t buffer_count_;
    uint64_t buffer_index_;
};
}  // namespace rdma
#endif  // RDMA_MEMORY_POOL_H
