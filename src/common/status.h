#pragma once
#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace tideskv {

enum class StatusCode : uint8_t {
  kOk = 0,
  kNotFound,
  kInvalidArgument,
  kBusy,      // transient: engine contention, retry later
  kConflict,  // transaction conflict
  kNoSpace,   // disk full
  kIOError,
  kCorruption,
  kInternal,  // a bug: invariant violated
};

std::string_view StatusCodeName(StatusCode code);

class [[nodiscard]] Status {
 public:
  Status() = default;

  static Status Ok() { return {}; }
  static Status NotFound(std::string msg = {}) { return {StatusCode::kNotFound, std::move(msg)}; }
  static Status InvalidArgument(std::string msg) { return {StatusCode::kInvalidArgument, std::move(msg)}; }
  static Status Busy(std::string msg) { return {StatusCode::kBusy, std::move(msg)}; }
  static Status Conflict(std::string msg) { return {StatusCode::kConflict, std::move(msg)}; }
  static Status NoSpace(std::string msg) { return {StatusCode::kNoSpace, std::move(msg)}; }
  static Status IOError(std::string msg) { return {StatusCode::kIOError, std::move(msg)}; }
  static Status Corruption(std::string msg) { return {StatusCode::kCorruption, std::move(msg)}; }
  static Status Internal(std::string msg) { return {StatusCode::kInternal, std::move(msg)}; }

  bool ok() const { return code_ == StatusCode::kOk; }
  bool IsNotFound() const { return code_ == StatusCode::kNotFound; }
  StatusCode code() const { return code_; }
  const std::string& message() const { return message_; }
  // "OK", "NotFound", or "Corruption: bad block".
  std::string ToString() const;

 private:
  Status(StatusCode code, std::string msg) : code_(code), message_(std::move(msg)) {}

  StatusCode code_ = StatusCode::kOk;
  std::string message_;
};

// Either a value of T or a non-OK Status.
template <typename T>
class [[nodiscard]] Result {
 public:
  Result(T value) : data_(std::move(value)) {}        // NOLINT(google-explicit-constructor)
  Result(Status status) : data_(std::move(status)) {  // NOLINT(google-explicit-constructor)
    assert(!std::get<Status>(data_).ok() && "Result must not hold an OK Status");
  }

  bool ok() const { return std::holds_alternative<T>(data_); }

  const Status& status() const {
    static const Status kOkStatus;
    return ok() ? kOkStatus : std::get<Status>(data_);
  }

  T& value() & { return std::get<T>(data_); }
  const T& value() const& { return std::get<T>(data_); }
  T&& value() && { return std::get<T>(std::move(data_)); }

  T& operator*() & { return value(); }
  const T& operator*() const& { return value(); }
  T* operator->() { return &value(); }
  const T* operator->() const { return &value(); }

 private:
  std::variant<T, Status> data_;
};

}  // namespace tideskv
