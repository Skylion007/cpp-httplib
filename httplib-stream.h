//
//  httplib-stream.h
//
//  C++20 coroutine extensions for cpp-httplib
//  Requires C++20 or later
//
//  Copyright (c) 2025 Yuji Hirose. All rights reserved.
//  MIT License
//
//  C++23 Migration Notes:
//  ----------------------
//  When C++23 is adopted, this header can be simplified:
//  - Replace custom Generator<T> with std::generator<T> from <generator>
//  - The Generator interface is designed to be compatible with std::generator
//  - stream::Result and stream::Get() can remain unchanged
//

#ifndef CPPHTTPLIB_HTTPLIB20_H
#define CPPHTTPLIB_HTTPLIB20_H

// Version check: requires C++20 or later
#if __cplusplus < 202002L
#error "httplib-stream.h requires C++20 or later"
#endif

#include "httplib.h"

#include <coroutine>
#include <cstring>
#include <iterator>
#include <string_view>
#include <utility>

// C++23 feature detection for std::generator
// When available, prefer std::generator over custom implementation
#if defined(__cpp_lib_generator) && __cpp_lib_generator >= 202207L
#include <generator>
#define CPPHTTPLIB_USE_STD_GENERATOR 1
#endif

namespace httplib {

//------------------------------------------------------------------------------
// Generator<T> - A C++20 coroutine-based generator
//------------------------------------------------------------------------------
//
// This is a simplified implementation compatible with C++23's std::generator.
// Key features:
//   - Lazy evaluation: values computed on-demand
//   - Range-based for loop support via iterators
//   - Move-only semantics (non-copyable)
//   - Exception propagation from coroutine body
//
// Usage:
//   Generator<int> range(int start, int end) {
//     for (int i = start; i < end; ++i) {
//       co_yield i;
//     }
//   }
//
//   for (int value : range(0, 10)) {
//     std::cout << value << '\n';
//   }
//
// C++23 Migration:
//   Replace with: using Generator = std::generator;
//   Or: #include <generator> and use std::generator directly
//

#ifndef CPPHTTPLIB_USE_STD_GENERATOR

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

  // Constructors
  Generator() noexcept : handle_(nullptr) {}
  explicit Generator(Handle handle) noexcept : handle_(handle) {}

  // Move-only semantics
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

  // Range interface
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

#else // CPPHTTPLIB_USE_STD_GENERATOR

// C++23: Use std::generator directly
template <typename T> using Generator = std::generator<T>;

#endif // CPPHTTPLIB_USE_STD_GENERATOR

//------------------------------------------------------------------------------
// Streaming client functions using Generator
//------------------------------------------------------------------------------

namespace detail {

// Coroutine that yields chunks from StreamHandle
// Works with both memory buffer mode and socket direct mode
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
// stream namespace - C++20 streaming API
//------------------------------------------------------------------------------
//
// Provides a convenient interface for streaming HTTP response bodies.
// Data is read directly from the socket without buffering.
//
// Usage:
//   auto result = httplib::stream::Get(client, "/large-file");
//   if (result) {
//     for (auto chunk : result.body()) {
//       process(chunk);
//     }
//   }
//

namespace stream {

// Result - wrapper for streaming HTTP responses
class Result {
public:
  Result() = default;

  explicit Result(ClientImpl::StreamHandle &&handle)
      : handle_(std::move(handle)) {}

  // Move-only semantics
  Result(Result &&) = default;
  Result &operator=(Result &&) = default;
  Result(const Result &) = delete;
  Result &operator=(const Result &) = delete;

  // Validity check
  bool is_valid() const { return handle_.is_valid(); }
  explicit operator bool() const { return is_valid(); }

  // Response metadata access
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

  // Get read error (socket direct mode only)
  Error read_error() const { return handle_.get_read_error(); }

  // Streaming body access - returns Generator for lazy iteration
  Generator<std::string_view> body(size_t chunk_size = 8192) {
    return detail::stream_body(std::move(handle_), chunk_size);
  }

  // Read entire body at once (convenience method)
  // Note: For large responses, prefer body() for memory efficiency
  std::string read_all() { return handle_.read_all(); }

private:
  ClientImpl::StreamHandle handle_;
};

//------------------------------------------------------------------------------
// Streaming HTTP request functions
//------------------------------------------------------------------------------
//
// stream::Get reads response body directly from the socket without buffering
// the entire response in memory. This is ideal for:
//   - Large file downloads
//   - Server-Sent Events (SSE)
//   - Any streaming API
//
// Note: The connection is not reused (Keep-Alive disabled) since socket
// ownership is transferred to StreamHandle. For repeated small requests
// where connection reuse matters, use client.Get() instead.
//
// Usage:
//   auto result = httplib::stream::Get(client, "/huge-file");
//   for (auto chunk : result.body()) {
//     write_to_file(chunk);
//   }
//

inline Result Get(Client &cli, const std::string &path) {
  return Result{cli.open_stream(path)};
}

inline Result Get(Client &cli, const std::string &path,
                  const Headers &headers) {
  return Result{cli.open_stream(path, headers)};
}

inline Result Get(Client &cli, const std::string &path, const Params &params) {
  return Result{cli.open_stream(append_query_params(path, params))};
}

inline Result Get(Client &cli, const std::string &path, const Params &params,
                  const Headers &headers) {
  return Result{cli.open_stream(append_query_params(path, params), headers)};
}

} // namespace stream

} // namespace httplib

#endif // CPPHTTPLIB_HTTPLIB20_H
