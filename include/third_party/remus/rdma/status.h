/*
 * Modifications Copyright (c) 2026 Copyright (C) 2026 Yuehao Hu, Jiatang Zhou,
 * Tianzheng Wang, and Keval Vora
 *
 * Copyright (c) 2024 Scalable Systems and Software Research Group, Lehigh
 * University
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#pragma once

#include <optional>
#include <sstream>
#include <string>

namespace remus::util {
enum StatusType {
    Ok,
    InternalError,
    Unavailable,
    Cancelled,
    NotFound,
    Unknown,
    AlreadyExists,
    FailedPrecondition,
    InvalidArgument,
    ResourceExhausted,
    Aborted,
    OutOfRange,
    StreamTerminated
};

struct Status {
    StatusType t;
    std::optional<std::string> message;

    static Status Ok() { return {StatusType::Ok, {}}; }

    template <typename T>
    Status operator<<(T t) {
        std::string curr = message ? message.value() : "";
        std::stringstream s;
        s << curr;
        s << t;
        message = s.str();
        return *this;
    }
};

template <class T>
struct StatusVal {
    Status status;
    std::optional<T> val;
};
}  // namespace remus::util
