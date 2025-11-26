# cpp-httplib C++20 Streaming API

This document describes the C++20 streaming extensions for cpp-httplib, providing a generator-like API for handling HTTP responses incrementally.

## Overview

The C++20 streaming API allows you to process HTTP response bodies chunk by chunk using C++20 coroutines, similar to Python's generators or C++23's `std::generator`. This is particularly useful for:

- **LLM/AI streaming responses** (e.g., ChatGPT, Claude, Ollama)
- **Server-Sent Events (SSE)**
- **Large file downloads** with progress tracking
- **Reverse proxy implementations**

## Requirements

- C++20 compiler with coroutine support
- Include `httplib20.h` (which includes `httplib.h`)

## Quick Start

```cpp
#include "httplib20.h"

int main() {
    httplib::Client cli("http://localhost:8080");
    
    // Get streaming response
    auto result = httplib::GetStream(cli, "/stream");
    
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

The `StreamHandle` struct provides direct control over streaming responses.

```cpp
// Open a stream
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
    
    // Or read all at once
    std::string body = handle.read_all();
}
```

#### StreamHandle Members

| Member | Type | Description |
|--------|------|-------------|
| `response` | `std::unique_ptr<Response>` | HTTP response with headers |
| `error` | `Error` | Error code if request failed |
| `is_valid()` | `bool` | Returns true if response is valid |
| `read(buf, len)` | `ssize_t` | Read up to `len` bytes, returns bytes read (0 at EOF, -1 on error) |
| `read_all()` | `std::string` | Read all remaining content |

### High-Level API: `GetStream()` and `StreamingResult`

The `httplib20.h` header provides a more ergonomic API using C++20 coroutines.

```cpp
#include "httplib20.h"

httplib::Client cli("http://localhost:8080");

// Simple GET
auto result = httplib::GetStream(cli, "/path");

// GET with custom headers
httplib::Headers headers = {{"Authorization", "Bearer token"}};
auto result = httplib::GetStream(cli, "/path", headers);
```

#### StreamingResult Members

| Member | Type | Description |
|--------|------|-------------|
| `operator bool()` | `bool` | Returns true if response is valid |
| `status()` | `int` | HTTP status code |
| `headers()` | `Headers&` | Response headers |
| `body(chunk_size)` | `Generator<std::string_view>` | Generator yielding body chunks |
| `read_all()` | `std::string` | Read entire body at once |

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
#include "httplib20.h"
#include <iostream>

int main() {
    httplib::Client cli("http://localhost:8080");
    auto result = httplib::GetStream(cli, "/events");
    
    if (!result) {
        std::cerr << "Connection failed\n";
        return 1;
    }
    
    std::cout << "Connected, receiving events...\n";
    
    std::string buffer;
    for (auto chunk : result.body(256)) {
        buffer += chunk;
        
        // Process complete SSE messages
        size_t pos;
        while ((pos = buffer.find("\n\n")) != std::string::npos) {
            std::string message = buffer.substr(0, pos);
            buffer.erase(0, pos + 2);
            
            std::cout << "Event: " << message << "\n";
        }
    }
    
    return 0;
}
```

### Example 2: LLM Streaming Response (Ollama-style)

```cpp
#include "httplib20.h"
#include <iostream>

int main() {
    httplib::Client cli("http://localhost:11434");
    
    // Note: For POST requests, use open_stream with custom request
    // This example shows the pattern for streaming responses
    
    httplib::Headers headers = {{"Content-Type", "application/json"}};
    auto result = httplib::GetStream(cli, "/api/generate", headers);
    
    if (result && result.status() == 200) {
        for (auto chunk : result.body(1024)) {
            // Each chunk may contain JSON like: {"response": "Hello"}
            std::cout << chunk << std::flush;
        }
    }
    
    return 0;
}
```

### Example 3: Large File Download with Progress

```cpp
#include "httplib20.h"
#include <fstream>
#include <iostream>

int main() {
    httplib::Client cli("http://example.com");
    auto result = httplib::GetStream(cli, "/large-file.zip");
    
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
#include "httplib20.h"

int main() {
    httplib::Server svr;
    
    svr.Get("/proxy/(.*)", [](const httplib::Request& req, httplib::Response& res) {
        httplib::Client upstream("http://backend:8080");
        auto handle = upstream.open_stream("/" + req.matches[1].str());
        
        if (!handle.is_valid()) {
            res.status = 502;
            return;
        }
        
        // Forward status and headers
        res.status = handle.response->status;
        for (const auto& h : handle.response->headers) {
            res.set_header(h.first, h.second);
        }
        
        // Stream body using chunked transfer
        res.set_chunked_content_provider(
            handle.response->get_header_value("Content-Type"),
            [handle = std::move(handle)](size_t, httplib::DataSink& sink) mutable {
                char buf[8192];
                ssize_t n = handle.read(buf, sizeof(buf));
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
}
```

## Comparison with Existing APIs

| Feature | `Client::Get()` | `open_stream()` | `GetStream()` |
|---------|----------------|-----------------|---------------|
| Headers available | After complete | Immediately | Immediately |
| Body reading | All at once | Incremental | Generator-based |
| Memory usage | Full body in RAM | Controlled | Controlled |
| C++ standard | C++11 | C++11 | C++20 |
| Best for | Small responses | Low-level control | Modern streaming |

## Current Limitations

> **Note:** The current implementation loads the entire response body into memory before streaming. True socket-level streaming (reading directly from the network) is planned for a future release.

This means:

- Memory usage is similar to `Client::Get()` for now
- The API is ready for future optimization
- Useful for header-first processing and chunked iteration patterns

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
- [httplib20.h](./httplib20.h) - C++20 extensions
