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
#ifndef RDMA_LISTENER_H
#define RDMA_LISTENER_H

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <atomic>
#include <map>
#include <set>
#include <thread>

#include "utils.h"

namespace rdma::internal {
class Listener {
    /// A functor for joining a thread on deletion
    struct thread_deleter {
        void operator()(std::thread* thread) {
            if (thread->joinable()) {
                thread->join();
            }
            free(thread);
        }
    };
    /// A thread that joins itself on deletion
    using thread_ptr = std::unique_ptr<std::thread, thread_deleter>;

    void CreateListeningEndpoint() {
        // Create a socket
        listener_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listener_fd_ < 0) {
            SPDLOG_FATAL("socket: {}", strerror(errno));
        }
        // Set the SO_REUSEADDR option
        int opt = 1;
        if (setsockopt(listener_fd_, SOL_SOCKET, SO_REUSEADDR, &opt,
                       sizeof(opt)) < 0) {
            SPDLOG_FATAL("setsockopt: {}", strerror(errno));
        }
        // Bind the socket to the address
        sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(self_.port);
        addr.sin_addr.s_addr = inet_addr(self_.ip.c_str());
        if (bind(listener_fd_, reinterpret_cast<sockaddr*>(&addr),
                 sizeof(addr)) < 0) {
            SPDLOG_FATAL("bind: {}", strerror(errno));
        }
        // Set the socket as non-blocking
        // MakeNonBlocking(listent_fd_);
        // Listen for incoming connections
        if (listen(listener_fd_, expected_clients_) < 0) {
            SPDLOG_FATAL("listen: {}", strerror(errno));
        }
    }

    void HandleConnectionRequests() {
        SPDLOG_DEBUG("Listening for incoming connections");
        sockaddr_in client_address;
        fd_set master_set, read_fds;
        FD_ZERO(&master_set);
        FD_SET(listener_fd_, &master_set);
        int max_fd = listener_fd_;
        std::set<int> client_sockets;
        struct timeval timeout;
        while (true) {
            if (connected_clients_ == expected_clients_) {
                for (int client_socket : client_sockets) {
                    close(client_socket);
                }
                close(listener_fd_);
                listener_fd_ = 0;
                stop_listening_ = true;
                return;
            }
            read_fds = master_set;
            // Set timeout for select
            timeout.tv_sec = 0;
            timeout.tv_usec = 100000;  // 100ms timeout
            // Use select to monitor file descriptors
            int activity =
                select(max_fd + 1, &read_fds, nullptr, nullptr, &timeout);
            SPDLOG_TRACE("Select returned: {}", activity);
            if (activity < 0 && errno != EINTR) {
                SPDLOG_FATAL("select: {}", strerror(errno));
            }
            if (activity == 0) {
                continue;
            }
            // Check for incoming connections
            if (FD_ISSET(listener_fd_, &read_fds)) {
                socklen_t client_len = sizeof(client_address);
                int client_fd = accept(
                    listener_fd_, reinterpret_cast<sockaddr*>(&client_address),
                    &client_len);
                if (client_fd < 0) {
                    SPDLOG_FATAL("accept: {}", strerror(errno));
                }
                // Add the client to the master set
                FD_SET(client_fd, &master_set);
                client_sockets.insert(client_fd);
                max_fd = std::max(max_fd, client_fd);
                char client_ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &client_address.sin_addr, client_ip,
                          INET_ADDRSTRLEN);
                SPDLOG_DEBUG("Accepted connection from: {} with fd: {:#x}",
                             client_ip, client_fd);
            }
            // Check for incoming data
            for (auto it = client_sockets.begin();
                 it != client_sockets.end();) {
                int client_socket = *it;
                if (FD_ISSET(client_socket, &read_fds)) {
                    SPDLOG_DEBUG("Received data from client: {}",
                                 client_socket);
                    ConnectionContext ctx;

                    ssize_t bytes =
                        recv(client_socket, &ctx, sizeof(ConnectionContext), 0);
                    if (bytes < 0) {
                        SPDLOG_FATAL("recv: {}", strerror(errno));
                    }
                    if (bytes == 0) {
                        close(client_socket);
                        FD_CLR(client_socket, &master_set);
                        it = client_sockets.erase(it);

                        // Recalculate max_fd
                        if (!client_sockets.empty()) {
                            max_fd = std::max(listener_fd_,
                                              *client_sockets.rbegin());
                        } else {
                            max_fd = listener_fd_;
                        }
                        continue;
                    } else {
                        assert(bytes == sizeof(ConnectionContext));
                        if (bytes != sizeof(ConnectionContext)) {
                            SPDLOG_FATAL("Received invalid connection context");
                        }
                        SPDLOG_TRACE(
                            "Received connection context: address: {:#x}, "
                            "rkey: {:#x}, qp_num: {:#x}, "
                            "lid: {:#x}, id: {} from client: {:#x}",
                            ctx.address, ctx.rkey, ctx.qp_num, ctx.lid, ctx.id,
                            client_socket);
                        // Accept the connection
                        ibv_cq* rcq = CreateCq(ctx_);
                        // ibv_cq *scq = CreateCq(ctx_);
                        ibv_qp* qp = CreateQp(rcq, rcq, pd_);
                        std::shared_ptr<TargetContext> target_ctx =
                            ConnectQp(rcq, rcq, qp, ctx, gid_index_, ib_port_);

                        SPDLOG_DEBUG(
                            "cq: {:#x}, qp: {:#x}, sizeof(cq): {}, sizeof(qp): "
                            "{}",
                            reinterpret_cast<uint64_t>(rcq),
                            reinterpret_cast<uint64_t>(qp), sizeof(ibv_cq),
                            sizeof(ibv_qp));
                        // Add the client to the map
                        if (clients_.find(ctx.id) != clients_.end()) {
                            SPDLOG_WARN("Client with id {} already exists",
                                        ctx.id);
                            connected_clients_--;
                        }
                        clients_[ctx.id] = target_ctx;
                        SPDLOG_DEBUG("Accepted rdma connection with id: {}",
                                     ctx.id);
                        ctx.address = reinterpret_cast<uint64_t>(mr_->addr);
                        ctx.rkey = mr_->rkey;
                        ctx.qp_num = qp->qp_num;
                        ctx.lid = lid_;
                        memcpy(ctx.gid, gid_->raw, 16);
                        ssize_t sent = send(client_socket, &ctx,
                                            sizeof(ConnectionContext), 0);
                        if (sent != sizeof(ConnectionContext)) {
                            SPDLOG_FATAL("Failed to send connection context");
                        }
                        SPDLOG_TRACE(
                            "Sent connection context: address: {:#x}, rkey: "
                            "{:#x}, "
                            "qp_num: {:#x}, "
                            "lid: {:#x}, id: {} to client: {:#x}",
                            ctx.address, ctx.rkey, ctx.qp_num, ctx.lid, ctx.id,
                            client_socket);
                        connected_clients_++;
                    }
                }
                ++it;
            }
        }
    }

   public:
    Listener(const Node& self, const int expected_clients, ibv_context* ctx,
             ibv_pd* pd, ibv_mr* mr, const uint16_t lid, const int gid_index,
             const ibv_gid* gid, const int ib_port = 1)
        : self_(self),
          expected_clients_(expected_clients),
          ctx_(ctx),
          connected_clients_(0),
          pd_(pd),
          mr_(mr),
          lid_(lid),
          gid_index_(gid_index),
          gid_(gid),
          ib_port_(ib_port) {
        SPDLOG_DEBUG(
            "Listener created with ip: {}, port: {}, expected clients: {}",
            self.ip, self.port, expected_clients);
    }

    void Start() {
        CreateListeningEndpoint();
        listener_thread_.reset(
            new std::thread(&Listener::HandleConnectionRequests, this));
    }

    void Wait() {
        if (listener_thread_) {
            listener_thread_->join();
        }
    }

    void ClearCache() { clients_.clear(); }

    std::map<uint32_t, std::shared_ptr<TargetContext>> GetClients() const {
        return clients_;
    }

    ~Listener() {}

   private:
    const Node self_;
    const int expected_clients_;
    std::atomic<int> connected_clients_;
    std::map<uint32_t, std::shared_ptr<TargetContext>> clients_;
    ibv_context* ctx_;
    ibv_pd* pd_;
    ibv_mr* mr_;
    std::atomic<bool> stop_listening_;
    thread_ptr listener_thread_;
    const int ib_port_;
    const uint16_t lid_;
    const ibv_gid* gid_;
    const int gid_index_;

    int listener_fd_ = 0;
};
}  // namespace rdma::internal

#endif  // RDMA_LISTENER_H