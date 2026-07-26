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
#ifndef BENCHMARK_RESULT_H
#define BENCHMARK_RESULT_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "foedus/uniform_random.hpp"

namespace benchmark {
class Result {
   public:
    explicit Result(const uint32_t thread_id = 0, const uint32_t max_size = 0)
        : random_(thread_id), max_size_(max_size), number_of_samples_(0) {
        samples_.resize(max_size_);
    }

    void operator<<(const uint64_t sample) {
        if (number_of_samples_ >= max_size_) {
            uint32_t k = random_.uniform_within(0, number_of_samples_ - 1);
            if (k < max_size_) {
                samples_[k] = sample;
            }
        } else {
            samples_[number_of_samples_] = sample;
        }
        number_of_samples_++;
    }

    void Reset() { number_of_samples_ = 0; }

    uint64_t Sum() {
        if (number_of_samples_ == 0) {
            return 0;
        }

        if (number_of_samples_ > max_size_) {
            number_of_samples_ = max_size_;
        }

        uint64_t sum = 0;

        for (uint32_t i = 0; i < number_of_samples_; ++i) {
            sum += samples_[i];
        }

        return sum;
    }

    double Mean() { return (double)Sum() / number_of_samples_; }

    Result operator+=(const Result& other) {
        uint32_t n = std::min(other.max_size_, other.number_of_samples_);
        for (uint32_t i = 0; i < n; ++i) {
            *this << other.samples_[i];
        }
        return *this;
    }

    uint64_t& operator[](const uint32_t index) { return samples_[index]; }

    uint64_t Percentile(double percentile) {
        if (number_of_samples_ == 0 || percentile < 0 || percentile > 100) {
            return 0;
        }
        percentile /= 100;

        if (number_of_samples_ > max_size_) {
            number_of_samples_ = max_size_;
        }

        Sort();

        return samples_[std::min(
            static_cast<uint32_t>(number_of_samples_ * percentile),
            number_of_samples_ - 1)];
    }

    uint64_t Median() { return Percentile(0.5); }

    uint64_t Max() {
        if (number_of_samples_ == 0) {
            return 0;
        }

        Sort();

        return samples_[number_of_samples_ - 1];
    }

    uint64_t Min() {
        if (number_of_samples_ == 0) {
            return 0;
        }

        Sort();

        return samples_[0];
    }

    double Variance() {
        if (number_of_samples_ == 0) {
            return 0;
        }

        if (number_of_samples_ > max_size_) {
            number_of_samples_ = max_size_;
        }

        double mean = Mean();
        double sum = 0;

        for (uint32_t i = 0; i < number_of_samples_; ++i) {
            uint64_t diff = samples_[i] - mean;
            sum += diff * diff;
        }

        return sum / number_of_samples_;
    }

    double StandardDeviation() { return std::sqrt(Variance()); }

    double CoefficientOfVariation() {
        double mean = Mean();
        if (mean == 0) {
            return 0;
        }
        return StandardDeviation() / mean;
    }

    uint32_t Size() { return std::min(number_of_samples_, max_size_); }

    ~Result() {}

   private:
    void Sort() {
        if (number_of_samples_ == 0) {
            return;
        }

        if (number_of_samples_ > max_size_) {
            number_of_samples_ = max_size_;
            sorted_ = false;
        }

        if (sorted_) {
            return;
        }

        std::sort(samples_.begin(), samples_.begin() + number_of_samples_);
        sorted_ = true;
    }

    foedus::assorted::UniformRandom random_;
    uint32_t max_size_;
    uint32_t number_of_samples_;
    std::vector<uint64_t> samples_;
    bool sorted_ = false;
};
}  // namespace benchmark
#endif  // BENCHMARK_RESULT_H