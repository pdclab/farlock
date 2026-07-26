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
#ifndef BENCHMARK_BENCHMARK_H
#define BENCHMARK_BENCHMARK_H

#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

#include "common.h"
#include "config.h"
#include "foedus/uniform_random.hpp"
#include "logging/logging.h"
#include "result.h"

namespace benchmark {
template <class LockHandler>
class Benchmark {
    static uint64_t RdtscpAcquire() {
        uint32_t lo, hi;
#if defined(__x86_64__) || defined(__i386__)
        asm volatile(
            "MFENCE\n\t"
            "RDTSCP\n\t"
            : "=a"(lo), "=d"(hi)::"%ecx");
#elif defined(__aarch64__) || defined(__arm__)
        asm volatile(
            "DMB ish\n\t"
            "MRS %0, CNTVCT_EL0\n\t"
            : "=r"(lo));
        hi = 0;  // ARM CNTVCT_EL0 is a 64-bit counter, but we use only the
                 // lower 32 bits
#endif
        return ((static_cast<uint64_t>(hi) << 32) + lo);
    }

    static uint64_t RdtscpRelease() {
        uint32_t lo, hi;
#if defined(__x86_64__) || defined(__i386__)
        asm volatile(
            "RDTSCP\n\t"
            "LFENCE\n\t"
            : "=a"(lo), "=d"(hi)::"%ecx");
#elif defined(__aarch64__) || defined(__arm__)
        asm volatile(
            "MRS %0, CNTVCT_EL0\n\t"
            "ISB\n\t"
            : "=r"(lo));
        hi = 0;  // ARM CNTVCT_EL0 is a 64-bit counter, but we use only the
                 // lower 32 bits
#endif
        return ((static_cast<uint64_t>(hi) << 32) + lo);
    }

    std::vector<rdma::Pointer> AllocateLocalLocks(
        const uint32_t locks_per_node) {
        std::vector<rdma::Pointer> local_locks;
        for (uint32_t i = 0; i < locks_per_node; ++i) {
            auto lock = rdma_manager_.AllocateShared();
            memset(lock.address(), 0, rdma_manager_.GetChunkSize());
            local_locks.push_back(lock);
        }
        return local_locks;
    }

    std::vector<rdma::Pointer> ExchangeLockTable(
        const std::vector<rdma::Pointer>& local_locks) {
        std::vector<rdma::Pointer> remote_locks;
        const uint32_t chunk_size = rdma_manager_.GetChunkSize();
        for (uint32_t i = 0; i < nodes_; ++i) {
            if (i == node_id_) {
                continue;
            }
            rdma::Pointer lock;
            if (i > node_id_) {
                rdma_manager_.GetMemoryPool(0)->Send(i, local_locks[0].raw());
                lock = rdma_manager_.GetMemoryPool(0)->Receive(i);
            } else {
                lock = rdma_manager_.GetMemoryPool(0)->Receive(i);
                rdma_manager_.GetMemoryPool(0)->Send(i, local_locks[0].raw());
            }
            for (uint32_t j = 0; j < local_locks.size(); ++j) {
                remote_locks.push_back(lock + j * chunk_size);
            }
        }
        return remote_locks;
    }

    static void CriticalSection(uint32_t csize) {
        for (uint32_t i = 0; i < csize; ++i) {
            asm volatile("");  // Prevent optimization
        }
    }

   public:
    Benchmark(const std::string config_file, uint16_t node_id)
        : node_id_(node_id),
          rdma_manager_(rdma::Manager::GetInstance()),
          benchmark_config_(Config::GetInstance().GetConfig().benchmark),
          lock_config_(Config::GetInstance().GetConfig().lock) {
        Config& config = Config::GetInstance();
        if (!config.LoadConfig(config_file)) {
            SPDLOG_FATAL("Failed to load config.");
        }
        config.SetNodeId(node_id);
        const rdma::RdmaConfig& rdma_config = config.GetConfig().rdma;
        nodes_ = rdma_config.available_nodes;
        if (node_id_ >= nodes_) {
            SPDLOG_DEBUG(
                "Node ID {} is greater than the number of available nodes {}.",
                node_id_, nodes_);
            SPDLOG_DEBUG("Skipping initialization.");
            return;
        }
        // Initialize the RDMA manager
        rdma_manager_.Init(rdma_config, benchmark_config_.threads_per_node);
        // Initialize the lock table
        // Allocate the locks
        if (benchmark_config_.distribution == "uniform") {
            SPDLOG_DEBUG("Uniform distribution.");
            local_locks_ = AllocateLocalLocks(benchmark_config_.locks_per_node);
            // Exchange the lock table
            remote_locks_ = ExchangeLockTable(local_locks_);
            local_percentage_ = benchmark_config_.local_percentage;
        } else if (benchmark_config_.distribution == "selective") {
            SPDLOG_DEBUG("Selective distribution.");
            if (node_id < benchmark_config_.lock_hosts) {
                local_locks_ =
                    AllocateLocalLocks(benchmark_config_.locks_per_node);
                local_percentage_ = 100;
                for (uint16_t i = benchmark_config_.lock_hosts; i < nodes_;
                     ++i) {
                    rdma_manager_.GetMemoryPool(0)->Send(i,
                                                         local_locks_[0].raw());
                }
            } else {
                local_percentage_ = 0;
                const uint32_t chunk_size = rdma_manager_.GetChunkSize();
                for (uint16_t i = 0; i < benchmark_config_.lock_hosts; ++i) {
                    rdma::Pointer lock =
                        rdma_manager_.GetMemoryPool(0)->Receive(i);
                    for (uint32_t j = 0; j < benchmark_config_.locks_per_node;
                         ++j) {
                        remote_locks_.push_back(lock + j * chunk_size);
                    }
                }
            }
        } else {
            SPDLOG_FATAL("Invalid lock distribution: {}",
                         benchmark_config_.distribution);
        }
    }

    void Run() {
        if (node_id_ >= nodes_) {
            return;
        }
        uint32_t n_threads = benchmark_config_.threads_per_node;
        if (n_threads == 0) {
            SPDLOG_ERROR("Threads per node must be greater than 0.");
            return;
        }
        std::barrier barrier(n_threads + 1);
        stop_ = false;
        sampling_flag_ = false;
        std::vector<std::thread> threads;
        counts_ = std::vector<std::vector<uint64_t>>(
            2, std::vector<uint64_t>(n_threads, 0));
        latencies_ = std::vector<std::vector<Result>>(
            2, std::vector<Result>(n_threads, Result(0, 0)));
#if !(defined(BATCH_TESTING) && BATCH_TESTING)
        SPDLOG_INFO("Benchmarking...");
#endif  // BATCH_TESTING
        for (uint32_t i = 0; i < n_threads; ++i) {
            threads.emplace_back(
                [this, n_threads, &barrier](
                    uint32_t thread_id,
                    std::shared_ptr<rdma::MemoryPool> pool) {
                    PinToCore(thread_id);
                    std::vector<Result> latency(
                        2, Result(thread_id, benchmark_config_.sample_number)),
                        tmp_latency(2, Result(thread_id,
                                              benchmark_config_.sample_number));
                    uint64_t count[2] = {0}, tmp_count[2] = {0};
                    foedus::assorted::UniformRandom rng(node_id_ * n_threads +
                                                        thread_id),
                        rngl(node_id_ * n_threads + thread_id),
                        rngr(node_id_ * n_threads + thread_id);
                    const uint32_t n_llocks = local_locks_.size() - 1;
                    const uint32_t n_rlocks = remote_locks_.size() - 1;
                    uint32_t csize = 0;
                    LockHandler lock_handler(pool, lock_config_);
                    barrier.arrive_and_wait();
                    while (!stop_) {
                        uint32_t lr = rng.uniform_within(0, 99);
                        rdma::Pointer lock;
                        if (lr < local_percentage_) {
                            // Local lock
                            lock =
                                local_locks_[rngl.uniform_within(0, n_llocks)];
                            csize =
                                benchmark_config_.local_critical_section_size;
                            lr = 0;
                        } else {
                            // Remote lock
                            lock =
                                remote_locks_[rngr.uniform_within(0, n_rlocks)];
                            csize =
                                benchmark_config_.remote_critical_section_size;
                            lr = 1;
                        }
                        uint64_t lock_time = RdtscpAcquire();
                        lock_handler.Lock(lock);
                        CriticalSection(csize);
                        lock_handler.Unlock(lock);
                        uint64_t unlock_time = RdtscpRelease();
                        if (sampling_flag_) {
                            latency[lr] << unlock_time - lock_time;
                            ++count[lr];
                        } else {
                            tmp_latency[lr] << unlock_time - lock_time;
                            ++tmp_count[lr];
                        }
                    }
#if !(defined(BATCH_TESTING) && BATCH_TESTING)
                    SPDLOG_INFO(
                        "Thread: {}, Sampled count: {}, Unsampled count: {}",
                        thread_id, count, tmp);
#endif  // BATCH_TESTING
                    counts_[0][thread_id] = count[0];
                    counts_[1][thread_id] = count[1];
                    latencies_[0][thread_id] = std::move(latency[0]);
                    latencies_[1][thread_id] = std::move(latency[1]);
                },
                i, rdma_manager_.GetMemoryPool(i));
        }

        barrier.arrive_and_wait();
        std::this_thread::sleep_for(
            std::chrono::seconds(benchmark_config_.warmup));
        sampling_flag_ = true;
        std::this_thread::sleep_for(
            std::chrono::seconds(benchmark_config_.duration));
        sampling_flag_ = false;
        std::this_thread::sleep_for(
            std::chrono::seconds(benchmark_config_.cooldown));
        stop_ = true;

        for (auto& thread : threads) {
            thread.join();
        }
#if !(defined(BATCH_TESTING) && BATCH_TESTING)
        SPDLOG_INFO("Done.");
#endif  // BATCH_TESTING
    }

    void TallyResults() {
        if (node_id_ >= nodes_) {
            return;
        }
        SPDLOG_INFO("Tallying results...");
        uint32_t n_threads = benchmark_config_.threads_per_node;
        if (node_id_ == 0) {
            // Sync
            for (uint16_t i = 1; i < nodes_; ++i) {
                rdma_manager_.GetMemoryPool(0)->Receive(i);
            }
            uint32_t total_threads = nodes_ * n_threads;
            // Throughput
            if (benchmark_config_.distribution == "uniform") {
                for (int i = 0; i < 2; ++i) {
                    throughput_[i] = Result(0, total_threads);
                    for (uint32_t j = 0; j < n_threads; ++j) {
                        throughput_[i] << counts_[i][j];
                    }
                    counts_[i].clear();
                }
                for (uint32_t i = 1; i < nodes_; ++i) {
                    for (int j = 0; j < 2; ++j) {
                        for (uint32_t k = 0; k < n_threads; ++k) {
                            uint64_t count =
                                rdma_manager_.GetMemoryPool(0)->Receive(i);
                            throughput_[j] << count;
                        }
                    }
                }

            } else if (benchmark_config_.distribution == "selective") {
                uint32_t local_threads =
                    benchmark_config_.lock_hosts * n_threads;
                uint32_t remote_threads = total_threads - local_threads;
                throughput_[0] = Result(0, local_threads);
                throughput_[1] = Result(0, remote_threads);
                for (uint32_t j = 0; j < n_threads; ++j) {
                    throughput_[0] << counts_[0][j];
                }
                counts_.clear();
                for (uint32_t i = 1; i < benchmark_config_.lock_hosts; ++i) {
                    for (uint32_t j = 0; j < n_threads; ++j) {
                        uint64_t count =
                            rdma_manager_.GetMemoryPool(0)->Receive(i);
                        throughput_[0] << count;
                    }
                }
                for (int i = benchmark_config_.lock_hosts; i < nodes_; ++i) {
                    for (uint32_t j = 0; j < n_threads; ++j) {
                        uint64_t count =
                            rdma_manager_.GetMemoryPool(0)->Receive(i);
                        throughput_[1] << count;
                    }
                }
            } else {
                SPDLOG_FATAL("Invalid lock distribution: {}",
                             benchmark_config_.distribution);
            }
            // Latency
            for (int i = 0; i < 2; ++i) {
                latency_[i] =
                    Result(0, total_threads * benchmark_config_.sample_number);
                for (uint32_t j = 0; j < n_threads; ++j) {
                    latency_[i] += latencies_[i][j];
                }
                latencies_[i].clear();
            }
            latencies_.clear();
            for (uint32_t i = 1; i < nodes_; ++i) {
                for (int j = 0; j < 2; ++j) {
                    for (uint32_t k = 0; k < n_threads; ++k) {
                        uint32_t samples =
                            rdma_manager_.GetMemoryPool(0)->Receive(i);
                        for (uint32_t l = 0; l < samples; ++l) {
                            uint64_t sample =
                                rdma_manager_.GetMemoryPool(0)->Receive(i);
                            latency_[j] << sample;
                        }
                    }
                }
            }
        } else {
            // Sync
            rdma_manager_.GetMemoryPool(0)->Send(0, 0);
            if (benchmark_config_.distribution == "uniform") {
                for (int i = 0; i < 2; ++i) {
                    for (uint32_t j = 0; j < n_threads; ++j) {
                        rdma_manager_.GetMemoryPool(0)->Send(0, counts_[i][j]);
                    }
                }
            } else if (benchmark_config_.distribution == "selective") {
                if (node_id_ < benchmark_config_.lock_hosts) {
                    for (uint32_t j = 0; j < n_threads; ++j) {
                        rdma_manager_.GetMemoryPool(0)->Send(0, counts_[0][j]);
                    }
                } else {
                    for (uint32_t j = 0; j < n_threads; ++j) {
                        rdma_manager_.GetMemoryPool(0)->Send(0, counts_[1][j]);
                    }
                }
            } else {
                SPDLOG_FATAL("Invalid lock distribution: {}",
                             benchmark_config_.distribution);
            }
            // Latency
            for (uint32_t k = 0; k < 2; ++k) {
                for (uint32_t i = 0; i < n_threads; ++i) {
                    uint32_t samples = latencies_[k][i].Size();
                    rdma_manager_.GetMemoryPool(0)->Send(0, samples);
                    for (uint32_t j = 0; j < samples; ++j) {
                        rdma_manager_.GetMemoryPool(0)->Send(
                            0, latencies_[k][i][j]);
                    }
                }
            }
        }
        SPDLOG_INFO("Finished tallying results.");
    }

    void PrintResults() {
        TallyResults();
        if (node_id_ != 0) {
            return;
        }
        std::cout << "Throughput:\n";
        std::cout << "samples, total, max, min, mean, std_dev, cv\n";
        double duration = benchmark_config_.duration;
        for (int i = 0; i < 2; ++i) {
            std::cout << throughput_[i].Size() << ", "
                      << throughput_[i].Sum() / duration << ", "
                      << throughput_[i].Max() / duration << ", "
                      << throughput_[i].Min() / duration << ", "
                      << throughput_[i].Mean() / duration << ", "
                      << throughput_[i].StandardDeviation() / duration << ", "
                      << throughput_[i].CoefficientOfVariation() << std::endl;
        }
        std::cout << "Latency:\n";
        std::cout << "samples, max, min, mean, std_dev, cv, p95, p99, p999, "
                     "p9999, p99999\n";
        for (int i = 0; i < 2; ++i) {
            std::cout << latency_[i].Size() << ", " << latency_[i].Max() << ", "
                      << latency_[i].Min() << ", " << latency_[i].Mean() << ", "
                      << latency_[i].StandardDeviation() << ", "
                      << latency_[i].CoefficientOfVariation() << ", "
                      << latency_[i].Percentile(95) << ", "
                      << latency_[i].Percentile(99) << ", "
                      << latency_[i].Percentile(99.9) << ", "
                      << latency_[i].Percentile(99.99) << ", "
                      << latency_[i].Percentile(99.999) << std::endl;
        }
    }
    ~Benchmark() {
        if (node_id_ >= nodes_) {
            return;
        }
        // Synchonize before exit because other nodes may still access the
        // local memory via RDMA
        for (uint32_t i = 0; i < nodes_; ++i) {
            if (i == node_id_) {
                continue;
            }
            if (i > node_id_) {
                rdma_manager_.GetMemoryPool(0)->Send(i, 0);
                rdma_manager_.GetMemoryPool(0)->Receive(i);
            } else {
                rdma_manager_.GetMemoryPool(0)->Receive(i);
                rdma_manager_.GetMemoryPool(0)->Send(i, 0);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

   private:
#if 0
    static constexpr size_t cpu_ids[] = {
        0,  2,  4,  6,  8,  10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30,
        32, 34, 36, 38, 1,  3,  5,  7,  9,  11, 13, 15, 17, 19, 21, 23,
        25, 27, 29, 31, 33, 35, 37, 39, 40, 42, 44, 46, 48, 50, 52, 54,
        56, 58, 60, 62, 64, 66, 68, 70, 72, 74, 76, 78, 41, 43, 45, 47,
        49, 51, 53, 55, 57, 59, 61, 63, 65, 67, 69, 71, 73, 75, 77, 79,
    };
#else
    static constexpr size_t cpu_ids[] = {
        0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15,
        16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};
#endif
    static void PinToCore(uint32_t thread_id) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cpu_ids[thread_id], &cpuset);
        pthread_t current_thread = pthread_self();
        if (pthread_setaffinity_np(current_thread, sizeof(cpu_set_t),
                                   &cpuset) != 0) {
            SPDLOG_ERROR("Failed to set thread affinity.");
        }
    }

    rdma::Manager& rdma_manager_;
    uint16_t node_id_;
    uint16_t nodes_;
    const LockConfig& lock_config_;
    const BenchmarkConfig& benchmark_config_;
    uint32_t local_percentage_;
    std::vector<rdma::Pointer> local_locks_;
    std::vector<rdma::Pointer> remote_locks_;
    std::atomic<bool> sampling_flag_;
    std::atomic<bool> stop_;
    std::vector<std::vector<uint64_t>> counts_;
    std::vector<std::vector<Result>> latencies_;
    Result latency_[2];
    Result throughput_[2];
};

}  // namespace benchmark
#endif  // BENCHMARK_BENCHMARK_H