# cpp-httplib C++20 Streaming API

This document describes the C++20 streaming extensions for cpp-httplib, providing a generator-like API for handling HTTP responses incrementally with **true socket-level streaming**.

> **Important Notes**:
>
> - **No Keep-Alive**: Each `stream::Get()` call uses a dedicated connection that is closed after the response is fully read. For connection reuse, use `Client::Get()`.
> - **Single iteration only**: The `body()` generator can only be iterated once. Calling `body()` again after iteration has no effect.
> - **Result is not thread-safe**: While `stream::Get()` can be called from multiple threads simultaneously, the returned `stream::Result` must be used from a single thread only.

## Overview

The C++20 streaming API allows you to process HTTP response bodies chunk by chunk using C++20 coroutines, similar to Python's generators or C++23's `std::generator`. Data is read directly from the network socket, enabling low-memory processing of large responses. This is particularly useful for:

- **LLM/AI streaming responses** (e.g., ChatGPT, Claude, Ollama)
- **Server-Sent Events (SSE)**
- **Large file downloads** with progress tracking
- **Reverse proxy implementations**

## API Layers

cpp-httplib provides multiple API layers for different use cases:

```text
┌─────────────────────────────────────────────┐
│  SSEClient (planned)                        │  ← SSE-specific, parsed events
│  - on_message(), on_event()                 │
│  - Auto-reconnect, Last-Event-ID            │
├─────────────────────────────────────────────┤
│  stream::Get() / stream::Result             │  ← C++20, Generator-based
│  - for (auto chunk : result.body())         │
├─────────────────────────────────────────────┤
│  open_stream() / StreamHandle               │  ← General-purpose streaming
│  - handle.read(buf, len)                    │
├─────────────────────────────────────────────┤
│  Client::Get()                              │  ← Traditional, full buffering
└─────────────────────────────────────────────┘
```

| Use Case | Recommended API |
|----------|----------------|
| SSE with auto-reconnect | SSEClient (planned) or `ssecli-stream.cc` example |
| LLM streaming (JSON Lines) | `stream::Get()` |
| Large file download | `stream::Get()` or `open_stream()` |
| Reverse proxy | `open_stream()` |
| Small responses with Keep-Alive | `Client::Get()` |

## Requirements

- C++20 compiler with coroutine support
- Include `httplib-stream.h` (which includes `httplib.h`)

## Quick Start

```cpp
#include "httplib-stream.h"

int main() {
    httplib::Client cli("http://localhost:8080");
    
    // Get streaming response
    auto result = httplib::stream::Get(cli, "/stream");
    
    if (result) {
        // Process response body in chunks
        for (auto chunk : result.body(4096)) {
            std::cout << chunk;  // Process each chunk as it arrives
        }
    }
    
    return 0;
}
```

## API Reference

### Low-Level API: `StreamHandle`

The `StreamHandle` struct provides direct control over streaming responses. It takes ownership of the socket connection and reads data directly from the network.

> **Note:** When using `open_stream()`, the connection is dedicated to streaming and **Keep-Alive is not supported**. For Keep-Alive connections, use `client.Get()` instead.

```cpp
// Open a stream (takes ownership of socket)
httplib::Client cli("http://localhost:8080");
auto handle = cli.open_stream("/path");

// Check validity
if (handle.is_valid()) {
    // Access response headers immediately
    int status = handle.response->status;
    auto content_type = handle.response->get_header_value("Content-Type");
    
    // Read body incrementally
    char buf[4096];
    ssize_t n;
    while ((n = handle.read(buf, sizeof(buf))) > 0) {
        process(buf, n);
    }
}
```

#### StreamHandle Members

| Member | Type | Description |
|--------|------|-------------|
| `response` | `std::unique_ptr<Response>` | HTTP response with headers |
| `error` | `Error` | Error code if request failed |
| `is_valid()` | `bool` | Returns true if response is valid |
| `is_socket_direct_mode()` | `bool` | Returns true (always direct socket reading) |
| `read(buf, len)` | `ssize_t` | Read up to `len` bytes directly from socket |

### High-Level API: `stream::Get()` and `stream::Result`

The `httplib-stream.h` header provides a more ergonomic API using C++20 coroutines.

```cpp
#include "httplib-stream.h"

httplib::Client cli("http://localhost:8080");

// Simple GET
auto result = httplib::stream::Get(cli, "/path");

// GET with custom headers
httplib::Headers headers = {{"Authorization", "Bearer token"}};
auto result = httplib::stream::Get(cli, "/path", headers);
```

#### stream::Result Members

| Member | Type | Description |
|--------|------|-------------|
| `operator bool()` | `bool` | Returns true if response is valid |
| `status()` | `int` | HTTP status code |
| `headers()` | `Headers&` | Response headers |
| `body(chunk_size)` | `Generator<std::string_view>` | Generator yielding body chunks |
| `read_error()` | `Error` | Get the last read error |
| `has_read_error()` | `bool` | Check if a read error occurred |

### Generator Class

The `httplib::Generator<T>` class is a C++20 coroutine-based generator, similar to `std::generator` (C++23).

```cpp
// Iterate over chunks
for (auto chunk : result.body(1024)) {
    // chunk is std::string_view
    process(chunk);
}
```

## Usage Examples

### Example 1: SSE (Server-Sent Events) Client

```cpp
#include "httplib-stream.h"
#include <iostream>

int main() {
    httplib::Client cli("http://localhost:1234");
    
    auto result = httplib::stream::Get(cli, "/events");
    if (!result) { return 1; }
    
    for (auto chunk : result.body()) {
        std::cout << chunk << std::flush;
    }
    
    return 0;
}
```

For a complete SSE client with auto-reconnection and event parsing, see `example/ssecli-stream.cc`.

### Example 2: LLM Streaming Response

```cpp
#include "httplib-stream.h"
#include <iostream>

int main() {
    httplib::Client cli("http://localhost:11434");  // Ollama
    
    auto result = httplib::stream::Get(cli, "/api/generate");
    
    if (result && result.status() == 200) {
        for (auto chunk : result.body()) {
            std::cout << chunk << std::flush;
        }
    }
    
    // Check for connection errors
    if (result.read_error() != httplib::Error::Success) {
        std::cerr << "Connection lost\n";
    }
    
    return 0;
}
```

### Example 3: Large File Download with Progress

```cpp
#include "httplib-stream.h"
#include <fstream>
#include <iostream>

int main() {
    httplib::Client cli("http://example.com");
    auto result = httplib::stream::Get(cli, "/large-file.zip");
    
    if (!result || result.status() != 200) {
        std::cerr << "Download failed\n";
        return 1;
    }
    
    std::ofstream file("download.zip", std::ios::binary);
    size_t total = 0;
    
    for (auto chunk : result.body(65536)) {  // 64KB chunks
        file.write(chunk.data(), chunk.size());
        total += chunk.size();
        std::cout << "\rDownloaded: " << (total / 1024) << " KB" << std::flush;
    }
    
    std::cout << "\nComplete!\n";
    return 0;
}
```

### Example 4: Reverse Proxy Streaming

```cpp
#include "httplib.h"

httplib::Server svr;

svr.Get("/proxy/(.*)", [](const httplib::Request& req, httplib::Response& res) {
    httplib::Client upstream("http://backend:8080");
    auto handle = upstream.open_stream("/" + req.matches[1].str());
    
    if (!handle.is_valid()) {
        res.status = 502;
        return;
    }
    
    res.status = handle.response->status;
    res.set_chunked_content_provider(
        handle.response->get_header_value("Content-Type"),
        [handle = std::move(handle)](size_t, httplib::DataSink& sink) mutable {
            char buf[8192];
            auto n = handle.read(buf, sizeof(buf));
            if (n > 0) {
                sink.write(buf, static_cast<size_t>(n));
                return true;
            }
            sink.done();
            return true;
        }
    );
});

svr.listen("0.0.0.0", 3000);
```

## Comparison with Existing APIs

| Feature | `Client::Get()` | `open_stream()` | `stream::Get()` |
|---------|----------------|-----------------|----------------|
| Headers available | After complete | Immediately | Immediately |
| Body reading | All at once | Direct from socket | Generator-based |
| Memory usage | Full body in RAM | Minimal (controlled) | Minimal (controlled) |
| Keep-Alive support | ✅ Yes | ❌ No | ❌ No |
| Compression | Auto-handled | Auto-handled | Auto-handled |
| C++ standard | C++11 | C++11 | C++20 |
| Best for | Small responses, Keep-Alive | Low-level streaming | Modern streaming |

## Features

- **True socket-level streaming**: Data is read directly from the network socket
- **Low memory footprint**: Only the current chunk is held in memory
- **Compression support**: Automatic decompression for gzip, brotli, and zstd
- **Chunked transfer**: Full support for chunked transfer encoding
- **SSL/TLS support**: Works with HTTPS connections
- **C++23 ready**: `Generator<T>` is compatible with `std::generator` interface

## Important Notes

### Keep-Alive Behavior

The streaming API (`stream::Get()` / `open_stream()`) takes ownership of the socket connection for the duration of the stream. This means:

- **Keep-Alive is not supported** for streaming connections
- The socket is closed when `StreamHandle` is destroyed
- For Keep-Alive scenarios, use the standard `client.Get()` API instead

```cpp
// Use for streaming (no Keep-Alive)
auto stream = httplib::stream::Get(cli, "/large-stream");

// Use for Keep-Alive connections
auto result = cli.Get("/api/data");  // Connection can be reused
```

## Building

Compile with C++20 support:

```bash
# GCC
g++ -std=c++20 -o myapp myapp.cpp -lpthread -lssl -lcrypto

# Clang
clang++ -std=c++20 -o myapp myapp.cpp -lpthread -lssl -lcrypto
```

## Related

- [Issue #2269](https://github.com/yhirose/cpp-httplib/issues/2269) - Original feature request
- [httplib.h](./httplib.h) - Main library
- [httplib-stream.h](./httplib-stream.h) - C++20 extensions
- [example/ssecli-stream.cc](./example/ssecli-stream.cc) - SSE client with auto-reconnection
