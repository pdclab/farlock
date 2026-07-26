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
#ifndef RDMA_CONNECTOR_H
#define RDMA_CONNECTOR_H

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <unordered_map>
#include <vector>

#include "utils.h"

namespace rdma::internal {
class Connector {
    // Minimum value, in us, for exponential backoff
    static constexpr uint32_t kMinBackoffUs = 100;

    // Maximum value, in us, for exponential backoff
    static constexpr uint32_t kMaxBackoffUs = 5000000;

   public:
    Connector(const Node& self, ibv_context* ctx, ibv_pd* pd, ibv_mr* mr,
              const uint16_t lid, const int gid_index, const ibv_gid* gid,
              const int ib_port = 1)
        : self_(self),
          pd_(pd),
          mr_(mr),
          lid_(lid),
          gid_index_(gid_index),
          gid_(gid),
          ib_port_(ib_port),
          ctx_(ctx) {
        SPDLOG_DEBUG("Connector created with ip: {}, port: {}", self.ip,
                     self.port);
    }

    ~Connector() {}

    // Create a loopback connection
    std::shared_ptr<TargetContext> ConnectLoopback() {
        ibv_cq* rcq = CreateCq(ctx_);
        // ibv_cq *scq = CreateCq(ctx_);
        ibv_qp* qp = CreateQp(rcq, rcq, pd_);
        ConnectionContext ctx = {0};
        ctx.address = reinterpret_cast<uint64_t>(mr_->addr);
        ctx.rkey = mr_->rkey;
        ctx.qp_num = qp->qp_num;
        ctx.lid = lid_;
        memcpy(ctx.gid, gid_->raw, 16);
        std::shared_ptr<TargetContext> target_ctx =
            ConnectQp(rcq, rcq, qp, ctx, gid_index_, ib_port_);
        SPDLOG_DEBUG("Loopback connection established");
        return target_ctx;
    }

    // Create a connection to a remote node
    std::shared_ptr<TargetContext> Connect(const Node& remote, uint32_t id) {
        // Create a socket
        int client_fd;
        if (connected_servers_.find(remote.ip) != connected_servers_.end()) {
            SPDLOG_DEBUG("Already connected to server: {}", remote.ip);
            client_fd = connected_servers_[remote.ip];
        } else {
            client_fd = socket(AF_INET, SOCK_STREAM, 0);
            if (client_fd < 0) {
                SPDLOG_FATAL("socket: {}", strerror(errno));
            }
            // Connect to the server
            sockaddr_in server_addr = {0};
            server_addr.sin_family = AF_INET;
            server_addr.sin_port = htons(remote.port);
            server_addr.sin_addr.s_addr = inet_addr(remote.ip.c_str());
            // Try to connect with exponential backoff
            while (true) {
                int ret = connect(client_fd,
                                  reinterpret_cast<sockaddr*>(&server_addr),
                                  sizeof(server_addr));
                if (ret == 0) {
                    break;
                }
                SPDLOG_TRACE("connect: {}", strerror(errno));
                backoff_us_ = backoff_us_ > 0
                                  ? std::min((backoff_us_ + (100 * id)) * 2,
                                             kMaxBackoffUs)
                                  : kMinBackoffUs;
                SPDLOG_TRACE("Retrying in {} us", backoff_us_);
                std::this_thread::sleep_for(
                    std::chrono::microseconds(backoff_us_));
            }
            SPDLOG_DEBUG("Connected to server: {}", remote.ip);
        }
        // Create a completion queue
        ibv_cq* rcq = CreateCq(ctx_);
        // ibv_cq *scq = CreateCq(ctx_);
        //  Create a queue pair
        ibv_qp* qp = CreateQp(rcq, rcq, pd_);

        SPDLOG_DEBUG("cq: {:#x}, qp: {:#x}", reinterpret_cast<uint64_t>(rcq),
                     reinterpret_cast<uint64_t>(qp));
        // Prepare the connection context
        ConnectionContext ctx = {0};
        ctx.address = reinterpret_cast<uint64_t>(mr_->addr);
        ctx.rkey = mr_->rkey;
        ctx.qp_num = qp->qp_num;
        ctx.lid = lid_;
        ctx.id = id;
        memcpy(ctx.gid, gid_->raw, 16);
        // Send the connection context
        ssize_t sent = send(client_fd, &ctx, sizeof(ConnectionContext), 0);
        if (sent != sizeof(ConnectionContext)) {
            SPDLOG_FATAL("Failed to send connection context");
        }
        SPDLOG_TRACE(
            "Sent connection context: address: {:#x}, rkey: {:#x}, qp_num: "
            "{:#x}, "
            "lid: {:#x}, gid.subnet_prefix: {:#x}, gid.interface_id: {:#x}, "
            "id: {}"
            " to server: {}",
            ctx.address, ctx.rkey, ctx.qp_num, ctx.lid,
            gid_->global.subnet_prefix, gid_->global.interface_id, ctx.id,
            remote.ip);
        // Receive the connection context
        ssize_t bytes = recv(client_fd, &ctx, sizeof(ConnectionContext), 0);
        if (bytes != sizeof(ConnectionContext)) {
            SPDLOG_FATAL("Received invalid connection context");
        }
        SPDLOG_TRACE(
            "Received connection context: address: {:#x}, rkey: {:#x}, qp_num: "
            "{:#x}, "
            "lid: {:#x}, id: {} from server: {}",
            ctx.address, ctx.rkey, ctx.qp_num, ctx.lid, ctx.id, remote.ip);

        std::shared_ptr<TargetContext> target_ctx =
            ConnectQp(rcq, rcq, qp, ctx, gid_index_, ib_port_);
        SPDLOG_DEBUG("Connection {} established with server: {}", id,
                     remote.ip);
        // Add the client to the map
        connected_servers_[remote.ip] = client_fd;
        return target_ctx;
    }

    void ClearCache() {
        for (auto& server : connected_servers_) {
            close(server.second);
        }
        connected_servers_.clear();
    }

   private:
    std::unordered_map<std::string, int> connected_servers_;
    const Node self_;
    const int ib_port_;
    const uint16_t lid_;
    ibv_context* ctx_;
    ibv_pd* pd_;
    const ibv_mr* mr_;
    const ibv_gid* gid_;
    const int gid_index_;
    uint32_t backoff_us_ = 0;
};
}  // namespace rdma::internal
#endif  // RDMA_CONNECTOR_H