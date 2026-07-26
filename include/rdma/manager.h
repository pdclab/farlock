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
#ifndef RDMA_MANAGER_H_
#define RDMA_MANAGER_H_
// #pragma once

#include <sys/mman.h>

#include <cstdlib>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "connector.h"
#include "listener.h"
#include "memory_pool.h"

namespace rdma {

class Manager {
    // Deleter for unique pointer to free the memory region
    struct mmap_deleter {
        size_t size;
        mmap_deleter(size_t s) : size(s) {}
        void operator()(void* raw) { munmap(raw, size); }
    };
    struct malloc_deleter {
        void operator()(void* ptr) { std::free(ptr); }
    };
    using mmap_unique_ptr = std::unique_ptr<void, mmap_deleter>;
    using malloc_unique_ptr = std::unique_ptr<void, malloc_deleter>;
    using rdma_unique_ptr = std::variant<malloc_unique_ptr, mmap_unique_ptr>;

    // Get the total free huge page size
    // Note: This function is only available on Linux
    static size_t GetTotalFreeHugePageSize() {
        std::ifstream meminfo("/proc/meminfo");
        if (!meminfo.is_open()) {
            SPDLOG_ERROR("Failed to open /proc/meminfo: {}", strerror(errno));
            return 0;
        }
        size_t hugepage_size = 0;
        size_t free_hugepages = 0;
        size_t size_uint = 1024;
        std::string line;
        while (std::getline(meminfo, line)) {
            if (line.find("Hugepagesize") != std::string::npos) {
                size_t pos = line.find_first_of("0123456789");
                if (pos != std::string::npos) {
                    hugepage_size = std::stoul(line.substr(pos));
                } else {
                    SPDLOG_ERROR(
                        "Failed to parse Hugepagesize from /proc/meminfo");
                }
                // Check if the size is in KB
                if (line.find("kB") == std::string::npos) {
                    SPDLOG_ERROR("Hugepagesize is not in KB: {}", line);
                    size_uint = 0;
                }
            } else if (line.find("HugePages_Free") != std::string::npos) {
                size_t pos = line.find_first_of("0123456789");
                if (pos != std::string::npos) {
                    free_hugepages = std::stoul(line.substr(pos));
                } else {
                    SPDLOG_ERROR(
                        "Failed to parse HugePages_Free from "
                        "/proc/meminfo");
                }
            }
        }
        return hugepage_size * free_hugepages * size_uint;
    }

    bool FindActivePort() {
        int num_devices;
        ibv_device** devs = ibv_get_device_list(&num_devices);
        if (num_devices == 0) {
            SPDLOG_FATAL("No RDMA devices found.");
        }
        gid_index_ = -1;
        // choose the first device whose port is active
        for (int i = 0; i < num_devices; ++i) {
            ibv_context* ctx = ibv_open_device(devs[i]);
            if (ctx) {
                ibv_device_attr dev_attr;
                if (ibv_query_device(ctx, &dev_attr)) {
                    SPDLOG_ERROR("device name: {}, ibv_query_device(): {}",
                                 devs[i]->name, strerror(errno));
                    continue;
                }
                for (int j = 1; j <= dev_attr.phys_port_cnt; ++j) {
                    ibv_port_attr port_attr;
                    if (ibv_query_port(ctx, j, &port_attr)) {
                        SPDLOG_ERROR(
                            "device name: {}, port: {}, ibv_query_port(): "
                            "{}",
                            devs[i]->name, j, strerror(errno));
                        continue;
                    }
                    if (port_attr.state == IBV_PORT_ACTIVE) {
                        SPDLOG_DEBUG("device name: {}, port: {} is active",
                                     devs[i]->name, j);
                        // get the first non-zero gid
                        for (int gid_index = 0;
                             gid_index < port_attr.gid_tbl_len; ++gid_index) {
                            union ibv_gid tmp_gid;
                            if (ibv_query_gid(ctx, j, gid_index, &tmp_gid)) {
                                SPDLOG_ERROR(
                                    "device name: {}, port: {}, gid: {}, "
                                    "ibv_query_gid(): {}",
                                    devs[i]->name, j, gid_index,
                                    strerror(errno));
                                continue;
                            }
                            if (tmp_gid.global.subnet_prefix == 0 &&
                                tmp_gid.global.interface_id == 0) {
                                continue;
                            }
                            gid_index_ = gid_index;
                            gid_ = tmp_gid;
                            SPDLOG_DEBUG(
                                "device name: {}, port: {}, gid: {} is found",
                                devs[i]->name, j, gid_index);
                            break;
                        }
                        if (gid_index_ == -1) {
                            SPDLOG_ERROR("No valid gid found for port: {}", j);
                            continue;
                        }
                        lid_ = port_attr.lid;
                        ib_port_ = j;
                        break;
                    }
                }
                if (ib_port_ == 0) {
                    SPDLOG_DEBUG("No active port found for device: {}",
                                 devs[i]->name);
                    ibv_close_device(ctx);
                } else {
                    SPDLOG_DEBUG("Active port found for device: {}",
                                 devs[i]->name);
                    ctx_ = ctx;
                    break;
                }
            }
        }
        // free the device list
        ibv_free_device_list(devs);
        return ctx_ != nullptr;
    }

    // Initialize the memory region
    inline void* InitMemoryRegion(size_t chunk_size, size_t desired_size) {
        // Create a protection domain
        pd_ = ibv_alloc_pd(ctx_);
        if (pd_ == nullptr) {
            SPDLOG_FATAL("device name: {}, ibv_alloc_pd(): {}",
                         ctx_->device->name, strerror(errno));
        }

        // align the chunk size to the power of 2
        chunk_size = 1 << (64 - __builtin_clzll(chunk_size - 1));
        if (chunk_size < internal::kMinChunkSize) {
            chunk_size = internal::kMinChunkSize;
            SPDLOG_INFO("Chunk size is too small, using the minimum size: {}",
                        chunk_size);
        } else if (chunk_size > internal::kMaxChunkSize) {
            chunk_size = internal::kMaxChunkSize;
            SPDLOG_INFO("Chunk size is too large, using the maximum size: {}",
                        chunk_size);
        }

        if (desired_size % chunk_size != 0) {
            SPDLOG_FATAL("Region size {} must be a multiple of chunk size {}",
                         desired_size, chunk_size);
        }

        SPDLOG_DEBUG("Chunk size: {}, region size: {}", chunk_size,
                     desired_size);

        void* region_base = nullptr;
        // allocate memory rigion
        // check if we can use huge pages
        size_t huge_page_size = GetTotalFreeHugePageSize();
        SPDLOG_DEBUG("Total free huge page size: {}", huge_page_size);
        if (huge_page_size >= desired_size) {
#if !(defined(BATCH_TESTING) && BATCH_TESTING)
            SPDLOG_INFO("Using huge pages for memory region");
#endif  // BATCH_TESTING
            region_base =
                mmap(nullptr, desired_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
            if (region_base == MAP_FAILED) {
                SPDLOG_FATAL("mmap(): {}", strerror(errno));
            }
            memory_region_ =
                mmap_unique_ptr(region_base, mmap_deleter(desired_size));
        } else {
#if !(defined(BATCH_TESTING) && BATCH_TESTING)
            SPDLOG_INFO("Using regular pages for memory region");
#endif  // BATCH_TESTING
            region_base = std::aligned_alloc(chunk_size, desired_size);
            if (region_base == nullptr) {
                SPDLOG_FATAL("aligned_alloc() failed");
            }
            memory_region_ = malloc_unique_ptr(region_base);
        }
        mr_ = ibv_reg_mr(pd_, region_base, desired_size,
                         internal::kDefaultAccessMode);
        if (mr_ == nullptr) {
            SPDLOG_FATAL("device name: {}, ibv_reg_mr(): {}",
                         ctx_->device->name, strerror(errno));
        }
        assert(mr_->addr == region_base);
        chunk_size_ = chunk_size;
        return region_base;
    }

    std::vector<internal::FreeChunk> InitFreeLists(void* start, size_t size,
                                                   uint32_t local_pools,
                                                   double shared_ratio) {
        std::vector<internal::FreeChunk> free_lists(local_pools);
        uint32_t chunk_num = size / chunk_size_;
        uint32_t unshared_chunk_num =
            static_cast<uint32_t>(chunk_num * (1 - shared_ratio));
        uint32_t chunk_per_thread = unshared_chunk_num / local_pools;
        uint32_t shared_chunk_num = chunk_num - unshared_chunk_num;
        SPDLOG_DEBUG("Chunk number: {}, shared chunk number: {}", chunk_num,
                     shared_chunk_num);
        // Initialize the shared free list
        free_list_.next = reinterpret_cast<internal::FreeChunk*>(start);
        internal::FreeChunk* current = free_list_.next;
        for (uint32_t i = 1; i < shared_chunk_num; ++i) {
            current->next = reinterpret_cast<internal::FreeChunk*>(
                reinterpret_cast<char*>(current) + chunk_size_);
            current = current->next;
        }
        current->next = nullptr;
        // Initialize the thread free lists
        for (uint32_t i = 0; i < local_pools; ++i) {
            current = reinterpret_cast<internal::FreeChunk*>(
                reinterpret_cast<char*>(current) + chunk_size_);
            free_lists[i].next = current;
            for (uint32_t j = 1; j < chunk_per_thread; ++j) {
                current->next = reinterpret_cast<internal::FreeChunk*>(
                    reinterpret_cast<char*>(current) + chunk_size_);
                current = current->next;
            }
            current->next = nullptr;
        }
        return free_lists;
    }

    void InitMemoryPool(const std::vector<internal::Node>& nodes,
                        const uint32_t local_pools,
                        std::vector<internal::FreeChunk> free_lists) {
        // Node with a smaller id connects to the node with a larger id
        listener_ = std::make_unique<internal::Listener>(
            nodes[node_id_], local_pools * node_id_, ctx_, pd_, mr_, lid_,
            gid_index_, &gid_, ib_port_);
        connector_ = std::make_unique<internal::Connector>(
            nodes[node_id_], ctx_, pd_, mr_, lid_, gid_index_, &gid_, ib_port_);
        listener_->Start();

        std::map<uint32_t, std::shared_ptr<TargetContext>> clients;
        // Connect to other nodes, starting from the next node
        for (uint32_t i = node_id_ + 1; i < nodes.size(); ++i) {
            for (uint32_t j = 0; j < local_pools; ++j) {
                uint32_t id = node_id_ * local_pools + j;
                clients[i * local_pools + j] =
                    connector_->Connect(nodes[i], id);
            }
        }
        listener_->Wait();
        for (uint32_t i = 0; i < local_pools; ++i) {
            std::vector<std::shared_ptr<TargetContext>> target_map;
            for (uint32_t j = 0; j < node_id_; ++j) {
                target_map.push_back(
                    listener_->GetClients()[j * local_pools + i]);
            }
            target_map.push_back(connector_->ConnectLoopback());
            for (uint32_t j = node_id_ + 1; j < nodes.size(); ++j) {
                target_map.push_back(clients[j * local_pools + i]);
            }
            thread_pools_.push_back(std::make_shared<MemoryPool>(
                free_lists[i], chunk_size_, node_id_, mr_->lkey, target_map));
        }
        listener_->ClearCache();
        connector_->ClearCache();
    }

   public:
    static Manager& GetInstance() {
        std::call_once(instance_init_flag,
                       [] { instance.reset(new Manager()); });
        return *instance;
    }

    void Init(const RdmaConfig& config, const uint32_t local_pools = 1) {
        assert(local_pools > 0);
        std::call_once(dev_init_flag, [&] {
            assert(config.node_id < config.nodes.size());
            node_id_ = config.node_id;
            // find the active port
            if (!FindActivePort()) {
                SPDLOG_FATAL("No active RDMA ports found.");
            }
            SPDLOG_DEBUG("RDMA device name: {}, port: {}", ctx_->device->name,
                         ib_port_);
            // Initialize the memory region
            size_t region_size = (size_t)config.region_size_mb * 1024 * 1024;
            void* region_base =
                InitMemoryRegion(config.chunk_size_byte, region_size);
            // Initialize the free lists
            std::vector<internal::FreeChunk> free_lists = InitFreeLists(
                region_base, region_size, local_pools, config.shared_ratio);
            // Establish RDMA connections
            std::vector<internal::Node> nodes;
            for (uint32_t i = 0; i < config.available_nodes; ++i) {
                nodes.push_back({config.nodes[i], config.port});
            }
            InitMemoryPool(nodes, local_pools, free_lists);
        });
    }

    Pointer AllocateShared() {
        internal::FreeChunk* chunk = free_list_.next;
        if (chunk == nullptr) {
            return nullptr;
        }
        free_list_.next = chunk->next;
        return Pointer(node_id_, chunk);
    }

    void FreeShared(Pointer& ptr) {
        internal::FreeChunk* chunk =
            reinterpret_cast<internal::FreeChunk*>(ptr.address());
        chunk->next = free_list_.next;
        free_list_.next = chunk;
    }

    std::shared_ptr<MemoryPool> GetMemoryPool(uint32_t thread_id) {
        return thread_pools_[thread_id % thread_pools_.size()];
    }

    size_t GetChunkSize() const { return chunk_size_; }

   private:
    Manager() = default;
    // Delete copy constructor and assignment operator
    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;
    ~Manager() {
        thread_pools_.clear();
        if (mr_) {
            ibv_dereg_mr(mr_);
        }
        if (pd_) {
            ibv_dealloc_pd(pd_);
        }
        if (ctx_) {
            ibv_close_device(ctx_);
        }
    }

    struct Deleter {
        void operator()(Manager* ptr) const { delete ptr; }
    };
    // Static unique pointer for the single instance
    static std::unique_ptr<Manager, Deleter> instance;
    // Flag to ensure initialization happens only once
    static std::once_flag instance_init_flag;
    std::once_flag dev_init_flag;

    rdma_unique_ptr memory_region_;
    internal::FreeChunk free_list_;
    size_t chunk_size_;
    std::vector<std::shared_ptr<MemoryPool>> thread_pools_;

    uint32_t node_id_;
    // RDMA attributes
    int ib_port_ = 0;
    uint16_t lid_;
    ibv_context* ctx_ = nullptr;
    ibv_pd* pd_ = nullptr;
    ibv_mr* mr_ = nullptr;
    std::unique_ptr<internal::Listener> listener_;
    std::unique_ptr<internal::Connector> connector_;
    int gid_index_;
    ibv_gid gid_;
};
// Initialize the static instance pointer
std::unique_ptr<Manager, Manager::Deleter> Manager::instance = nullptr;
// Initialize the static flag
std::once_flag Manager::instance_init_flag;
}  // namespace rdma
#endif  // RDMA_MANAGER_H_