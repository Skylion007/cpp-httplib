//
//  httplib20.h
//
//  C++20 coroutine extensions for cpp-httplib
//  Requires C++20 or later
//
//  Copyright (c) 2025 Yuji Hirose. All rights reserved.
//  MIT License
//

#ifndef CPPHTTPLIB_HTTPLIB20_H
#define CPPHTTPLIB_HTTPLIB20_H

#if __cplusplus < 202002L
#error "httplib20.h requires C++20 or later"
#endif

#include "httplib.h"
#include <coroutine>
#include <cstring>
#include <iterator>
#include <string_view>
#include <utility>

namespace httplib {

//------------------------------------------------------------------------------
// Generator<T> - A C++20 coroutine-based generator (similar to std::generator)
//------------------------------------------------------------------------------

template <typename T> class Generator {
public:
  struct promise_type {
    T current_value_;
    std::exception_ptr exception_;

    Generator get_return_object() {
      return Generator{Handle::from_promise(*this)};
    }

    std::suspend_always initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }

    std::suspend_always yield_value(T value) noexcept {
      current_value_ = std::move(value);
      return {};
    }

    void return_void() noexcept {}

    void unhandled_exception() { exception_ = std::current_exception(); }

    void rethrow_if_exception() {
      if (exception_) { std::rethrow_exception(exception_); }
    }
  };

  using Handle = std::coroutine_handle<promise_type>;

  // Iterator for range-based for loop support
  class Iterator {
  public:
    using iterator_category = std::input_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = T;
    using pointer = const T *;
    using reference = const T &;

    Iterator() noexcept : handle_(nullptr) {}
    explicit Iterator(Handle handle) noexcept : handle_(handle) {}

    reference operator*() const noexcept {
      return handle_.promise().current_value_;
    }
    pointer operator->() const noexcept {
      return &handle_.promise().current_value_;
    }

    Iterator &operator++() {
      handle_.resume();
      if (handle_.done()) {
        handle_.promise().rethrow_if_exception();
        handle_ = nullptr;
      }
      return *this;
    }

    Iterator operator++(int) {
      Iterator tmp = *this;
      ++(*this);
      return tmp;
    }

    friend bool operator==(const Iterator &a, const Iterator &b) noexcept {
      return a.handle_ == b.handle_;
    }
    friend bool operator!=(const Iterator &a, const Iterator &b) noexcept {
      return !(a == b);
    }

  private:
    Handle handle_;
  };

  Generator() noexcept : handle_(nullptr) {}

  explicit Generator(Handle handle) noexcept : handle_(handle) {}

  Generator(Generator &&other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
  }

  Generator &operator=(Generator &&other) noexcept {
    if (this != &other) {
      if (handle_) { handle_.destroy(); }
      handle_ = other.handle_;
      other.handle_ = nullptr;
    }
    return *this;
  }

  Generator(const Generator &) = delete;
  Generator &operator=(const Generator &) = delete;

  ~Generator() {
    if (handle_) { handle_.destroy(); }
  }

  Iterator begin() {
    if (handle_) {
      handle_.resume();
      if (handle_.done()) {
        handle_.promise().rethrow_if_exception();
        return Iterator{};
      }
    }
    return Iterator{handle_};
  }

  Iterator end() noexcept { return Iterator{}; }

  // Check if generator has more values
  explicit operator bool() const noexcept { return handle_ && !handle_.done(); }

private:
  Handle handle_;
};

//------------------------------------------------------------------------------
// Streaming client functions using Generator
//------------------------------------------------------------------------------

namespace detail {

// Coroutine that yields chunks from StreamHandle
inline Generator<std::string_view> stream_body(ClientImpl::StreamHandle handle,
                                               size_t chunk_size = 8192) {
  if (!handle.is_valid()) { co_return; }

  std::string buffer(chunk_size, '\0');
  ssize_t n;

  while ((n = handle.read(buffer.data(), buffer.size())) > 0) {
    co_yield std::string_view(buffer.data(), static_cast<size_t>(n));
  }
}

} // namespace detail

//------------------------------------------------------------------------------
// StreamingResult - Wrapper for streaming response with Generator support
//------------------------------------------------------------------------------

class StreamingResult {
public:
  StreamingResult() = default;

  explicit StreamingResult(ClientImpl::StreamHandle &&handle)
      : handle_(std::move(handle)) {}

  StreamingResult(StreamingResult &&) = default;
  StreamingResult &operator=(StreamingResult &&) = default;

  StreamingResult(const StreamingResult &) = delete;
  StreamingResult &operator=(const StreamingResult &) = delete;

  // Check if result is valid
  bool is_valid() const { return handle_.is_valid(); }
  explicit operator bool() const { return is_valid(); }

  // Access response metadata
  int status() const {
    return handle_.response ? handle_.response->status : -1;
  }

  const Headers &headers() const {
    static const Headers empty_headers;
    return handle_.response ? handle_.response->headers : empty_headers;
  }

  std::string get_header_value(const std::string &key,
                               const char *def = "") const {
    return handle_.response ? handle_.response->get_header_value(key, def)
                            : def;
  }

  bool has_header(const std::string &key) const {
    return handle_.response ? handle_.response->has_header(key) : false;
  }

  Error error() const { return handle_.error; }

  // Get body as Generator for streaming reads
  Generator<std::string_view> body(size_t chunk_size = 8192) {
    return detail::stream_body(std::move(handle_), chunk_size);
  }

  // Read entire body at once (convenience method)
  std::string read_all() {
    if (!handle_.is_valid()) { return {}; }
    return handle_.response ? std::move(handle_.response->body) : std::string{};
  }

private:
  ClientImpl::StreamHandle handle_;
};

//------------------------------------------------------------------------------
// Free functions for streaming requests
//------------------------------------------------------------------------------

// GET request with streaming response
inline StreamingResult GetStream(Client &cli, const std::string &path) {
  return StreamingResult{cli.open_stream(path)};
}

inline StreamingResult GetStream(Client &cli, const std::string &path,
                                 const Headers &headers) {
  return StreamingResult{cli.open_stream(path, headers)};
}

// Overloads for ClientImpl
inline StreamingResult GetStream(ClientImpl &cli, const std::string &path) {
  return StreamingResult{cli.open_stream(path)};
}

inline StreamingResult GetStream(ClientImpl &cli, const std::string &path,
                                 const Headers &headers) {
  return StreamingResult{cli.open_stream(path, headers)};
}

} // namespace httplib

#endif // CPPHTTPLIB_HTTPLIB20_H
