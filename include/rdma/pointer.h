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
#ifndef RDMA_POINTER_H
#define RDMA_POINTER_H
// #pragma once

#include <iostream>

#include "utils.h"

namespace rdma {
class Pointer {
   public:
    Pointer() : raw_(0) {}

    Pointer(const std::nullptr_t) : raw_(0) {}

    Pointer(const uint64_t raw) : raw_(raw) {}

    Pointer(const Pointer& other) : raw_(other.raw_) {}

    Pointer(const uint16_t id, const void* address)
        : raw_(static_cast<uint64_t>(id) << 48 |
               reinterpret_cast<uint64_t>(address)) {}

    Pointer& operator=(const Pointer& other) {
        raw_ = other.raw_;
        return *this;
    }

    Pointer& operator=(const uint64_t raw) {
        raw_ = raw;
        return *this;
    }

    Pointer& operator=(const std::nullptr_t) {
        raw_ = 0;
        return *this;
    }

    bool operator==(const std::nullptr_t) const { return raw_ == 0; }

    bool operator!=(const std::nullptr_t) const { return raw_ != 0; }

    Pointer& operator+=(const size_t offset) {
        raw_ += offset;
        return *this;
    }

    Pointer operator+(const size_t offset) const {
        return Pointer(raw_ + offset);
    }

    bool operator==(const Pointer& other) const { return raw_ == other.raw_; }

    bool operator!=(const Pointer& other) const { return raw_ != other.raw_; }

    operator bool() const { return raw_ != 0; }

    uint16_t id() const { return raw_ >> 48; }

    void* address() const {
        return reinterpret_cast<void*>(raw_ & 0x0000FFFFFFFFFFFF);
    }

    uint64_t raw() const { return raw_; }

    friend std::ostream& operator<<(std::ostream& os, const Pointer& p);

    ~Pointer() {}

   private:
    uint64_t raw_;
};

std::ostream& operator<<(std::ostream& os, const Pointer& p) {
    os << "Pointer<" << p.id() << ", 0x" << std::hex << p.address() << std::dec
       << ">";
    return os;
}
}  // namespace rdma
#endif  // RDMA_POINTER_H