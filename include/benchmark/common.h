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
#ifndef BENCHMARK_COMMON_H
#define BENCHMARK_COMMON_H
#include "logging/logging.h"

void ParseArgs(int argc, char* argv[], uint16_t& node_id,
               std::string& config_file) {
    if (argc < 2) {
        SPDLOG_FATAL("Usage: {} <node_id> <config file path>", argv[0]);
    }
    try {
        node_id = std::stoi(argv[1]);
    } catch (const std::invalid_argument& e) {
        SPDLOG_FATAL("Invalid node id: {}", argv[1]);
    }
    if (node_id < 0) {
        SPDLOG_FATAL("Node id must be non-negative: {}", node_id);
    }
    if (argc < 3) {
        config_file = "../config/config.json";
    } else {
        config_file = argv[2];
    }
}
#endif  // BENCHMARK_COMMON_H