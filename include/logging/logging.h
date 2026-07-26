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
#ifndef AFLOCK_LOGGING_H
#define AFLOCK_LOGGING_H
// #pragma once
#include <spdlog/async.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#ifndef SPDLOG_ACTIVE_LEVEL
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#endif

#define BATCH_TESTING 1

inline void InitLog(bool async = false) {
    auto logger = ::spdlog::get("aflock");
    if (!logger) {
        if (async) {
            ::spdlog::init_thread_pool(8192, 1);
            logger =
                ::spdlog::create_async<::spdlog::sinks::stdout_color_sink_mt>(
                    "aflock");
        } else {
            logger = ::spdlog::create<::spdlog::sinks::stdout_color_sink_mt>(
                "aflock");
        }
        static_assert(SPDLOG_ACTIVE_LEVEL < spdlog::level::level_enum::n_levels,
                      "Invalid logging level.");
        logger->set_level(
            static_cast<spdlog::level::level_enum>(SPDLOG_ACTIVE_LEVEL));
        logger->set_pattern(
            "[%m-%d %H:%M:%S.%f thread:%t] [%^%l%$] [%s:%#] %v");
        ::spdlog::set_default_logger(std::move(logger));
#if !(defined(BATCH_TESTING) && BATCH_TESTING)
        SPDLOG_INFO("Logging level: {}",
                    ::spdlog::level::to_string_view(
                        ::spdlog::default_logger()->level()));
#endif  // BATCH_TESTING
    }
}
#if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_DEBUG
#define SPDLOG_FATAL(...)             \
    do {                              \
        SPDLOG_CRITICAL(__VA_ARGS__); \
        assert(0);                    \
        while (1);                    \
    } while (0)
#else
#define SPDLOG_FATAL(...)             \
    do {                              \
        SPDLOG_CRITICAL(__VA_ARGS__); \
        std::_Exit(1);                \
    } while (0)
#endif
#endif  // AFLOCK_LOGGING_H