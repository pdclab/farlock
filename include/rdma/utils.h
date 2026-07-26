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
#ifndef RDMA_INTERNAL_H
#define RDMA_INTERNAL_H
// #pragma once

#include <fcntl.h>
#include <infiniband/verbs.h>

#include <cstddef>
#include <cstdint>
#include <memory>

#include "logging/logging.h"

namespace rdma {
// Cache line size
constexpr uint32_t kCacheLineSize = 64;

struct RdmaConfig {
    uint32_t node_id = 0;
    std::vector<std::string> nodes;
    uint32_t available_nodes = 0;
    uint16_t port;
    uint32_t region_size_mb;
    uint32_t chunk_size_byte;
    double shared_ratio;
};

struct alignas(kCacheLineSize) TargetContext {
   public:
    TargetContext() = delete;
    ~TargetContext() {
        if (qp_) {
            ibv_destroy_qp(qp_);
        }
        if (send_cq_) {
            ibv_destroy_cq(send_cq_);
        }
        if (recv_cq_ && recv_cq_ != send_cq_) {
            ibv_destroy_cq(recv_cq_);
        }
    }
    TargetContext(const TargetContext&) = delete;
    TargetContext(ibv_cq* recv_cq, ibv_cq* send_cq, ibv_qp* qp,
                  const uint64_t address, const uint32_t rkey)
        : qp_(qp),
          recv_cq_(recv_cq),
          send_cq_(send_cq),
          rkey_(rkey),
          address_(address) {}
    const uint32_t rkey_;
    const uint64_t address_;  // will later be used as lock table address
    ibv_cq* recv_cq_;
    ibv_cq* send_cq_;
    ibv_qp* qp_;
};
}  // namespace rdma

namespace rdma::internal {

struct Node {
    std::string ip;
    uint16_t port;
};

union FreeChunk {
    FreeChunk* next = nullptr;
    uint8_t data[0];
};

#pragma pack(push, 1)
struct ConnectionContext {
    uint64_t address;
    uint32_t id;  // node id * thread_per_node + thread_id
    uint32_t rkey;
    uint32_t qp_num; /* QP number */
    uint16_t lid;    /* LID of the IB port */
    uint8_t gid[16]; /* gid */
};
#pragma pack(pop)

// Default thread count is 1
constexpr uint32_t kDefaultThreadNum = 1;

// Minimun chunk size is 64 Bytes
constexpr size_t kMinChunkSize = 1 << 6;

// Maximum chunk size is 4 KiB
constexpr size_t kMaxChunkSize = 1 << 12;

// Default RDMA memory access mode: local write, remote read, remote write,
// remote atomic
constexpr int kDefaultAccessMode =
    IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE |
    IBV_ACCESS_REMOTE_ATOMIC;

// Default RDMA region size is 10 MiB
constexpr size_t kDefaultRegionSize = 1 << 21;

// Default shared memory size is 1 MiB
constexpr size_t kDefaultSharedSize = 1 << 20;

// Max # SGEs in a single RDMA write
constexpr int kMaxSge = 1;

// We aren't using INLINE data
constexpr int kMaxInlineData = 0;

// Max message size
constexpr int kMaxRecvBytes = 64;

// Max # outstanding writes
constexpr int kMaxWr = kMaxChunkSize / kMaxRecvBytes;

constexpr int kCqSize = 256;

// Set the file descriptor `fd` as O_NONBLOCK
inline void MakeNonBlocking(int fd) {
    using namespace std::string_literals;
    if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) != 0) {
        SPDLOG_FATAL("fcntl():"s + strerror(errno));
    }
}

// Set the file descriptor `fd` as O_SYNC
inline void MakeSync(int fd) {
    using namespace std::string_literals;
    if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_SYNC) != 0) {
        SPDLOG_FATAL("fcntl():"s + strerror(errno));
    }
}

// Configure the minimum attributes for a QP
inline ibv_qp_init_attr DefaultQpInitAttr() {
    ibv_qp_init_attr init_attr = {0};
    init_attr.cap.max_send_wr = init_attr.cap.max_recv_wr = kCqSize;
    init_attr.cap.max_send_sge = init_attr.cap.max_recv_sge = 1;
    init_attr.cap.max_inline_data = kMaxInlineData;
    init_attr.sq_sig_all = 0;  // Must request completions.
    init_attr.qp_type = IBV_QPT_RC;
    return init_attr;
}

inline void ModifyQpToInit(struct ibv_qp* qp, int ib_port = 1) {
    struct ibv_qp_attr attr;
    int flags;

    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_INIT;
    attr.port_num = ib_port;
    attr.pkey_index = 0;
    attr.qp_access_flags = kDefaultAccessMode;
    flags =
        IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
    if (ibv_modify_qp(qp, &attr, flags)) {
        SPDLOG_FATAL("Failed to modify QP to INIT: {}", strerror(errno));
    }
}

inline void ModifyQpToRtr(struct ibv_qp* qp, const uint32_t remote_qpn,
                          const uint16_t dlid, const uint8_t* dgid,
                          const int sgid_index, const int ib_port = 1) {
    struct ibv_qp_attr attr;
    int flags;

    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_4096;
    attr.dest_qp_num = remote_qpn;
    attr.rq_psn = 0;
    attr.max_dest_rd_atomic = 8;
    attr.min_rnr_timer = 12;
    attr.ah_attr.is_global = 0;
    attr.ah_attr.dlid = dlid;
    attr.ah_attr.sl = 0;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.port_num = ib_port;
    if (sgid_index >= 0) {
        attr.ah_attr.is_global = 1;
        attr.ah_attr.port_num = 1;
        memcpy(&attr.ah_attr.grh.dgid, dgid, 16);
        attr.ah_attr.grh.flow_label = 0;
        attr.ah_attr.grh.hop_limit = 1;
        attr.ah_attr.grh.sgid_index = sgid_index;
        attr.ah_attr.grh.traffic_class = 0;
    }
    flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
            IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    if (ibv_modify_qp(qp, &attr, flags)) {
        SPDLOG_FATAL("Failed to modify QP to RTR: {}", strerror(errno));
    }
}

inline void ModifyQpToRts(struct ibv_qp* qp) {
    struct ibv_qp_attr attr;
    int flags;

    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 12;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;  // infinite
    attr.sq_psn = 0;
    attr.max_rd_atomic = 8;
    flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
            IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
    if (ibv_modify_qp(qp, &attr, flags)) {
        SPDLOG_FATAL("Failed to modify QP to RTS: {}", strerror(errno));
    }
}

inline ibv_cq* CreateCq(ibv_context* ibv_ctx) {
    ibv_cq* cq = ibv_create_cq(ibv_ctx, kCqSize, nullptr, nullptr, 0);
    if (cq == nullptr) {
        SPDLOG_FATAL("ibv_create_cq: {}", strerror(errno));
    }
    return cq;
}

inline ibv_qp* CreateQp(ibv_cq* recv_cq, ibv_cq* send_cq, ibv_pd* pd) {
    ibv_qp_init_attr init_attr = DefaultQpInitAttr();
    init_attr.recv_cq = recv_cq;
    init_attr.send_cq = send_cq;
    ibv_qp* qp = ibv_create_qp(pd, &init_attr);
    if (qp == nullptr) {
        SPDLOG_FATAL("ibv_create_qp: {}", strerror(errno));
    }
    return qp;
}

inline std::shared_ptr<TargetContext> ConnectQp(ibv_cq* rcq, ibv_cq* scq,
                                                ibv_qp* qp,
                                                const ConnectionContext& ctx,
                                                const int sgid_index,
                                                const int ib_port = 1) {
    ModifyQpToInit(qp);
    ModifyQpToRtr(qp, ctx.qp_num, ctx.lid, ctx.gid, sgid_index, ib_port);
    ModifyQpToRts(qp);
    return std::make_shared<TargetContext>(rcq, scq, qp, ctx.address, ctx.rkey);
}

const char* StrError(int err) {
    switch (err) {
        case EINVAL:
            return "Invalid value provided in wr";
        case ENOMEM:
            return "Send Queue is full or not enough resources to complete "
                   "this operation";
        case EFAULT:
            return "Invalid value provided in qp";
        default:
            return "Unknown error";
    }
}

}  // namespace rdma::internal

#endif  // RDMA_INTERNAL_H
