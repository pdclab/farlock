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
#ifndef CONFIG_CONFIG_H_
#define CONFIG_CONFIG_H_
// #pragma once
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <vector>

#include "locks/common.h"
#include "rdma/manager.h"

namespace benchmark {

using RdmaConfig = rdma::RdmaConfig;
using LockConfig = lock::LockConfig;
struct BenchmarkConfig {
    uint32_t locks_per_node;
    std::string distribution;  // uniform: every node has the same number
                               // of locks
                               // selective: some nodes listed in
                               // the lock_hosts have locks
    uint32_t lock_hosts;       // effective in selective distribution
    uint32_t threads_per_node;
    uint32_t sample_number;
    uint32_t local_percentage;  // effecive in uniform distribution
    uint32_t local_critical_section_size;
    uint32_t remote_critical_section_size;
    uint32_t warmup;
    uint32_t duration;
    uint32_t cooldown;
    uint32_t sampling_interval;
};

class Config {
   public:
    struct ConfigData {
        RdmaConfig rdma;
        LockConfig lock;
        BenchmarkConfig benchmark;
    };

    static Config& GetInstance() {
        std::call_once(init_flag, [] { instance.reset(new Config()); });
        return *instance;
    }

    bool LoadConfig(const std::string& file_path) {
        std::ifstream file(file_path);
        if (!file.is_open()) {
            std::cerr << "Failed to open config file: " << file_path
                      << std::endl;
            return false;
        }

        nlohmann::json json;
        try {
            file >> json;
        } catch (const nlohmann::json::parse_error& e) {
            std::cerr << "JSON parse error: " << e.what() << std::endl;
            return false;
        }

        try {
            for (const auto& node : json.at("rdma").at("nodes")) {
                config_data.rdma.nodes.push_back(node.get<std::string>());
            }

            config_data.rdma.port = json.at("rdma").at("port");
            config_data.rdma.available_nodes =
                json.at("rdma").at("available_nodes");
            config_data.rdma.region_size_mb =
                json.at("rdma").at("region_size_mb");
            config_data.rdma.chunk_size_byte =
                json.at("rdma").at("chunk_size_byte");
            config_data.rdma.shared_ratio = json.at("rdma").at("shared_ratio");

            config_data.lock.local_budget_ = json.at("lock").at("local_budget");
            config_data.lock.remote_budget_ =
                json.at("lock").at("remote_budget");

            config_data.benchmark.locks_per_node =
                json.at("benchmark").at("locks_per_node");
            config_data.benchmark.distribution =
                json.at("benchmark").at("distribution");
            config_data.benchmark.lock_hosts =
                json.at("benchmark").at("lock_hosts");
            config_data.benchmark.threads_per_node =
                json.at("benchmark").at("threads_per_node");
            config_data.benchmark.sample_number =
                json.at("benchmark").at("sample_number");
            config_data.benchmark.local_percentage =
                json.at("benchmark").at("local_percentage");
            config_data.benchmark.local_critical_section_size =
                json.at("benchmark").at("local_critical_section_size");
            config_data.benchmark.remote_critical_section_size =
                json.at("benchmark").at("remote_critical_section_size");
            config_data.benchmark.warmup = json.at("benchmark").at("warmup");
            config_data.benchmark.duration =
                json.at("benchmark").at("duration");
            config_data.benchmark.cooldown =
                json.at("benchmark").at("cooldown");
            config_data.benchmark.sampling_interval =
                json.at("benchmark").at("sampling_interval");

        } catch (const nlohmann::json::out_of_range& e) {
            std::cerr << "JSON key error: " << e.what() << std::endl;
            return false;
        } catch (const nlohmann::json::type_error& e) {
            std::cerr << "JSON type error: " << e.what() << std::endl;
            return false;
        }

        return true;
    }

    const ConfigData& GetConfig() const { return config_data; }

    void SetNodeId(int id) { config_data.rdma.node_id = id; }

   private:
    Config() = default;
    ~Config() = default;
    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;
    // Custom deleter that allows std::unique_ptr to destroy the instance
    struct Deleter {
        void operator()(Config* ptr) const { delete ptr; }
    };
    static std::unique_ptr<Config, Deleter> instance;
    static std::once_flag init_flag;
    ConfigData config_data;
};
// Define the static members
std::unique_ptr<Config, Config::Deleter> Config::instance;
std::once_flag Config::init_flag;
}  // namespace benchmark
#endif  // CONFIG_CONFIG_H_