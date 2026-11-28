#pragma once

#include <cerrno>
#include <optional>
#include <string>
#include <utility>

namespace pw {

// Error carrying an errno code and a short human context. Used instead of
// exceptions across system-call and thread boundaries.
struct Error {
    int code = 0;
    std::string context;
};

// Result<T>: either a value or an Error. System-call paths never throw.
template <typename T>
class Result {
public:
    Result(T value) : value_(std::move(value)) {}
    Result(Error error) : error_(std::move(error)) {}

    static Result ok(T value) { return Result(std::move(value)); }
    static Result err(int code, std::string context) {
        return Result(Error{code, std::move(context)});
    }

    bool ok() const { return value_.has_value(); }
    explicit operator bool() const { return ok(); }

    T& value() { return *value_; }
    const T& value() const { return *value_; }
    T value_or(T fallback) const { return value_ ? *value_ : std::move(fallback); }

    const Error& error() const { return error_; }

private:
    std::optional<T> value_;
    Error error_;
};

}  // namespace pw
