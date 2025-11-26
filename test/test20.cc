// C++20 Streaming API Tests
// Requires C++20 or later

#include <gtest/gtest.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "../httplib.h"

//------------------------------------------------------------------------------
// Step 1: StreamHandle struct existence test
//------------------------------------------------------------------------------

TEST(StreamHandleTest, StructExists) {
  // Verify StreamHandle struct is defined and accessible
  httplib::ClientImpl::StreamHandle handle;

  // Check default state
  EXPECT_FALSE(handle.is_valid());
  EXPECT_EQ(httplib::Error::Success, handle.error);
  EXPECT_EQ(nullptr, handle.response);
}

TEST(StreamHandleTest, IsValidReturnsFalseWhenResponseIsNull) {
  httplib::ClientImpl::StreamHandle handle;
  handle.error = httplib::Error::Success;
  handle.response = nullptr;

  EXPECT_FALSE(handle.is_valid());
}

TEST(StreamHandleTest, IsValidReturnsFalseWhenErrorIsSet) {
  httplib::ClientImpl::StreamHandle handle;
  handle.error = httplib::Error::Connection;
  handle.response = std::make_unique<httplib::Response>();

  EXPECT_FALSE(handle.is_valid());
}

TEST(StreamHandleTest, IsValidReturnsTrueWhenValid) {
  httplib::ClientImpl::StreamHandle handle;
  handle.error = httplib::Error::Success;
  handle.response = std::make_unique<httplib::Response>();

  EXPECT_TRUE(handle.is_valid());
}

//------------------------------------------------------------------------------
// Step 2: open_stream() method test
//------------------------------------------------------------------------------

// Test server for streaming tests
class StreamingServerTest : public ::testing::Test {
protected:
  void SetUp() override {
    server_.Get("/hello", [](const httplib::Request &, httplib::Response &res) {
      res.set_content("Hello World!", "text/plain");
    });

    server_.Get(
        "/chunked", [](const httplib::Request &, httplib::Response &res) {
          res.set_chunked_content_provider(
              "text/plain", [](size_t offset, httplib::DataSink &sink) {
                if (offset < 3) {
                  std::string chunk = "chunk" + std::to_string(offset) + "\n";
                  sink.write(chunk.data(), chunk.size());
                  return true;
                }
                sink.done();
                return true;
              });
        });

    // Start server in a separate thread
    server_thread_ =
        std::thread([this]() { server_.listen("localhost", 8787); });

    // Wait for server to start
    while (!server_.is_running()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  void TearDown() override {
    server_.stop();
    if (server_thread_.joinable()) { server_thread_.join(); }
  }

  httplib::Server server_;
  std::thread server_thread_;
};

TEST_F(StreamingServerTest, OpenStreamMethodExists) {
  httplib::Client cli("localhost", 8787);

  // Verify open_stream method exists and returns StreamHandle
  auto handle = cli.open_stream("/hello");

  // Should be valid for successful request
  EXPECT_TRUE(handle.is_valid());
  EXPECT_EQ(200, handle.response->status);
}

TEST_F(StreamingServerTest, OpenStreamReturnsHeaders) {
  httplib::Client cli("localhost", 8787);

  auto handle = cli.open_stream("/hello");

  ASSERT_TRUE(handle.is_valid());
  EXPECT_TRUE(handle.response->has_header("Content-Type"));
  EXPECT_EQ("text/plain", handle.response->get_header_value("Content-Type"));
}

TEST_F(StreamingServerTest, OpenStreamWithInvalidPath) {
  httplib::Client cli("localhost", 8787);

  auto handle = cli.open_stream("/nonexistent");

  // Should still be valid but with 404 status
  EXPECT_TRUE(handle.is_valid());
  EXPECT_EQ(404, handle.response->status);
}

TEST_F(StreamingServerTest, OpenStreamConnectionError) {
  httplib::Client cli("localhost", 9999); // No server on this port

  auto handle = cli.open_stream("/hello");

  EXPECT_FALSE(handle.is_valid());
  EXPECT_NE(httplib::Error::Success, handle.error);
}

//------------------------------------------------------------------------------
// Step 3: StreamHandle::read() method test
//------------------------------------------------------------------------------

TEST_F(StreamingServerTest, ReadMethodExists) {
  httplib::Client cli("localhost", 8787);

  auto handle = cli.open_stream("/hello");
  ASSERT_TRUE(handle.is_valid());

  // Read should return data
  char buf[1024];
  auto n = handle.read(buf, sizeof(buf));

  EXPECT_GT(n, 0);
}

TEST_F(StreamingServerTest, ReadReturnsCorrectContent) {
  httplib::Client cli("localhost", 8787);

  auto handle = cli.open_stream("/hello");
  ASSERT_TRUE(handle.is_valid());

  std::string body;
  char buf[1024];
  ssize_t n;
  while ((n = handle.read(buf, sizeof(buf))) > 0) {
    body.append(buf, static_cast<size_t>(n));
  }

  EXPECT_EQ("Hello World!", body);
}

TEST_F(StreamingServerTest, ReadReturnsZeroAtEnd) {
  httplib::Client cli("localhost", 8787);

  auto handle = cli.open_stream("/hello");
  ASSERT_TRUE(handle.is_valid());

  // Read all content
  char buf[1024];
  while (handle.read(buf, sizeof(buf)) > 0) {
    // consume
  }

  // Further reads should return 0
  auto n = handle.read(buf, sizeof(buf));
  EXPECT_EQ(0, n);
}

TEST_F(StreamingServerTest, ReadSmallBuffer) {
  httplib::Client cli("localhost", 8787);

  auto handle = cli.open_stream("/hello");
  ASSERT_TRUE(handle.is_valid());

  // Read with small buffer
  std::string body;
  char buf[4]; // Small buffer
  ssize_t n;
  while ((n = handle.read(buf, sizeof(buf))) > 0) {
    body.append(buf, static_cast<size_t>(n));
  }

  EXPECT_EQ("Hello World!", body);
}

//------------------------------------------------------------------------------
// Step 4: httplib20.h Generator API tests
//------------------------------------------------------------------------------

#include "../httplib20.h"

TEST_F(StreamingServerTest, GetStreamReturnsStreamingResult) {
  httplib::Client cli("localhost", 8787);

  auto result = httplib::GetStream(cli, "/hello");

  EXPECT_TRUE(result.is_valid());
  EXPECT_EQ(200, result.status());
}

TEST_F(StreamingServerTest, GetStreamWithHeaders) {
  httplib::Client cli("localhost", 8787);

  auto result = httplib::GetStream(cli, "/hello");

  ASSERT_TRUE(result.is_valid());
  EXPECT_TRUE(result.has_header("Content-Type"));
  EXPECT_EQ("text/plain", result.get_header_value("Content-Type"));
}

TEST_F(StreamingServerTest, GetStreamBodyGenerator) {
  httplib::Client cli("localhost", 8787);

  auto result = httplib::GetStream(cli, "/hello");
  ASSERT_TRUE(result.is_valid());

  std::string body;
  for (auto chunk : result.body()) {
    body.append(chunk);
  }

  EXPECT_EQ("Hello World!", body);
}

TEST_F(StreamingServerTest, GetStreamBodyGeneratorSmallChunks) {
  httplib::Client cli("localhost", 8787);

  auto result = httplib::GetStream(cli, "/hello");
  ASSERT_TRUE(result.is_valid());

  std::string body;
  size_t chunk_count = 0;
  for (auto chunk : result.body(4)) { // Small chunk size
    body.append(chunk);
    chunk_count++;
  }

  EXPECT_EQ("Hello World!", body);
  EXPECT_GT(chunk_count, 1u); // Should have multiple chunks
}

TEST_F(StreamingServerTest, GetStreamReadAll) {
  httplib::Client cli("localhost", 8787);

  auto result = httplib::GetStream(cli, "/hello");
  ASSERT_TRUE(result.is_valid());

  std::string body = result.read_all();
  EXPECT_EQ("Hello World!", body);
}

TEST_F(StreamingServerTest, GetStreamConnectionError) {
  httplib::Client cli("localhost", 9999); // No server

  auto result = httplib::GetStream(cli, "/hello");

  EXPECT_FALSE(result.is_valid());
  EXPECT_NE(httplib::Error::Success, result.error());
}

TEST_F(StreamingServerTest, GetStream404) {
  httplib::Client cli("localhost", 8787);

  auto result = httplib::GetStream(cli, "/nonexistent");

  EXPECT_TRUE(result.is_valid());
  EXPECT_EQ(404, result.status());
}

TEST(GeneratorTest, EmptyGenerator) {
  auto gen = []() -> httplib::Generator<int> { co_return; }();

  int count = 0;
  for (auto val : gen) {
    (void)val;
    count++;
  }
  EXPECT_EQ(0, count);
}

TEST(GeneratorTest, SimpleGenerator) {
  auto gen = []() -> httplib::Generator<int> {
    co_yield 1;
    co_yield 2;
    co_yield 3;
  }();

  std::vector<int> values;
  for (auto val : gen) {
    values.push_back(val);
  }

  EXPECT_EQ(3u, values.size());
  EXPECT_EQ(1, values[0]);
  EXPECT_EQ(2, values[1]);
  EXPECT_EQ(3, values[2]);
}

//------------------------------------------------------------------------------
// Step 5: Integration tests with chunked transfer encoding
//------------------------------------------------------------------------------

class ChunkedStreamingTest : public ::testing::Test {
protected:
  void SetUp() override {
    svr_.Get("/chunked", [](const httplib::Request &, httplib::Response &res) {
      res.set_chunked_content_provider(
          "text/plain", [](size_t offset, httplib::DataSink &sink) {
            // Simulate streaming data in chunks
            if (offset == 0) {
              sink.write("chunk1\n", 7);
              return true;
            } else if (offset == 7) {
              sink.write("chunk2\n", 7);
              return true;
            } else if (offset == 14) {
              sink.write("chunk3\n", 7);
              sink.done();
              return true;
            }
            return false;
          });
    });

    svr_.Get("/large", [](const httplib::Request &, httplib::Response &res) {
      // Generate 100KB of data in chunks
      res.set_chunked_content_provider(
          "application/octet-stream",
          [](size_t offset, httplib::DataSink &sink) {
            const size_t total_size = 100 * 1024; // 100KB
            const size_t chunk_size = 1024;       // 1KB chunks

            if (offset >= total_size) {
              sink.done();
              return true;
            }

            std::string chunk(chunk_size, 'X');
            sink.write(chunk.data(), chunk.size());
            return true;
          });
    });

    svr_.Get("/sse-like", [](const httplib::Request &, httplib::Response &res) {
      // Simulate SSE/LLM streaming response
      res.set_chunked_content_provider(
          "text/event-stream",
          [count = 0](size_t /*offset*/, httplib::DataSink &sink) mutable {
            if (count < 5) {
              std::string event =
                  "data: message " + std::to_string(count) + "\n\n";
              sink.write(event.data(), event.size());
              count++;
              return true;
            }
            sink.done();
            return true;
          });
    });

    thread_ = std::thread([this]() { svr_.listen("127.0.0.1", 8787); });
    svr_.wait_until_ready();
  }

  void TearDown() override {
    svr_.stop();
    if (thread_.joinable()) { thread_.join(); }
  }

  httplib::Server svr_;
  std::thread thread_;
};

TEST_F(ChunkedStreamingTest, ReadChunkedResponse) {
  httplib::Client cli("http://127.0.0.1:8787");
  auto handle = cli.open_stream("/chunked");

  ASSERT_TRUE(handle.is_valid());
  EXPECT_EQ(200, handle.response->status);
  EXPECT_EQ("text/plain", handle.response->get_header_value("Content-Type"));

  // Read all content
  std::string body = handle.read_all();
  EXPECT_EQ("chunk1\nchunk2\nchunk3\n", body);
}

TEST_F(ChunkedStreamingTest, ReadChunkedResponseInPieces) {
  httplib::Client cli("http://127.0.0.1:8787");
  auto handle = cli.open_stream("/chunked");

  ASSERT_TRUE(handle.is_valid());

  // Read in small pieces
  std::vector<std::string> pieces;
  char buf[8];
  ssize_t n;
  while ((n = handle.read(buf, sizeof(buf))) > 0) {
    pieces.emplace_back(buf, static_cast<size_t>(n));
  }

  // Verify we got the content (may be in different chunk sizes)
  std::string combined;
  for (const auto &p : pieces) {
    combined += p;
  }
  EXPECT_EQ("chunk1\nchunk2\nchunk3\n", combined);
}

TEST_F(ChunkedStreamingTest, LargeResponseStreaming) {
  httplib::Client cli("http://127.0.0.1:8787");
  auto handle = cli.open_stream("/large");

  ASSERT_TRUE(handle.is_valid());
  EXPECT_EQ(200, handle.response->status);

  // Read in chunks and verify
  size_t total_read = 0;
  char buf[4096];
  ssize_t n;
  while ((n = handle.read(buf, sizeof(buf))) > 0) {
    // Verify content is all 'X'
    for (size_t i = 0; i < static_cast<size_t>(n); i++) {
      EXPECT_EQ('X', buf[i]);
    }
    total_read += static_cast<size_t>(n);
  }

  EXPECT_EQ(100u * 1024u, total_read); // 100KB total
}

TEST_F(ChunkedStreamingTest, SSELikeStreaming) {
  httplib::Client cli("http://127.0.0.1:8787");
  auto handle = cli.open_stream("/sse-like");

  ASSERT_TRUE(handle.is_valid());
  EXPECT_EQ(200, handle.response->status);
  EXPECT_EQ("text/event-stream",
            handle.response->get_header_value("Content-Type"));

  std::string body = handle.read_all();

  // Verify SSE format
  EXPECT_NE(std::string::npos, body.find("data: message 0"));
  EXPECT_NE(std::string::npos, body.find("data: message 4"));
}

TEST_F(ChunkedStreamingTest, GeneratorWithChunkedResponse) {
  httplib::Client cli("http://127.0.0.1:8787");
  auto result = httplib::GetStream(cli, "/chunked");

  ASSERT_TRUE(result);
  EXPECT_EQ(200, result.status());

  std::vector<std::string> chunks;
  for (auto chunk : result.body(8)) {
    chunks.emplace_back(chunk);
  }

  // Combine and verify
  std::string combined;
  for (const auto &c : chunks) {
    combined += c;
  }
  EXPECT_EQ("chunk1\nchunk2\nchunk3\n", combined);
}

TEST_F(ChunkedStreamingTest, GeneratorWithLargeResponse) {
  httplib::Client cli("http://127.0.0.1:8787");
  auto result = httplib::GetStream(cli, "/large");

  ASSERT_TRUE(result);

  size_t total_size = 0;
  size_t chunk_count = 0;
  for (auto chunk : result.body(4096)) {
    total_size += chunk.size();
    chunk_count++;
  }

  EXPECT_EQ(100u * 1024u, total_size); // 100KB total
  EXPECT_GT(chunk_count, 1u);          // Multiple chunks
}

TEST_F(ChunkedStreamingTest, SSELikeWithGenerator) {
  httplib::Client cli("http://127.0.0.1:8787");
  auto result = httplib::GetStream(cli, "/sse-like");

  ASSERT_TRUE(result);

  std::string combined;
  for (auto chunk : result.body(256)) {
    combined += chunk;
  }

  // Verify all messages received
  for (int i = 0; i < 5; i++) {
    std::string expected = "data: message " + std::to_string(i);
    EXPECT_NE(std::string::npos, combined.find(expected));
  }
}

//------------------------------------------------------------------------------
// Phase 2.1: ClientConnection class tests
//------------------------------------------------------------------------------

TEST(ClientConnectionTest, StructExists) {
  // Verify ClientConnection struct is defined
  httplib::ClientConnection conn;

  // Check default state
  EXPECT_EQ(INVALID_SOCKET, conn.sock);
  EXPECT_FALSE(conn.is_open());
}

TEST(ClientConnectionTest, IsOpenReturnsTrueWhenSocketValid) {
  httplib::ClientConnection conn;
  conn.sock = 1; // Fake valid socket

  EXPECT_TRUE(conn.is_open());
}

TEST(ClientConnectionTest, MoveConstructor) {
  httplib::ClientConnection conn1;
  conn1.sock = 42;

  httplib::ClientConnection conn2(std::move(conn1));

  EXPECT_EQ(42, conn2.sock);
  EXPECT_EQ(INVALID_SOCKET, conn1.sock); // Moved-from state
}

TEST(ClientConnectionTest, MoveAssignment) {
  httplib::ClientConnection conn1;
  conn1.sock = 42;

  httplib::ClientConnection conn2;
  conn2 = std::move(conn1);

  EXPECT_EQ(42, conn2.sock);
  EXPECT_EQ(INVALID_SOCKET, conn1.sock); // Moved-from state
}

//------------------------------------------------------------------------------
// Phase 2.2: BodyReader struct tests
//------------------------------------------------------------------------------

TEST(BodyReaderTest, StructExists) {
  // Verify BodyReader struct is defined
  httplib::detail::BodyReader reader;

  // Check default state
  EXPECT_EQ(nullptr, reader.stream);
  EXPECT_EQ(0u, reader.content_length);
  EXPECT_EQ(0u, reader.bytes_read);
  EXPECT_FALSE(reader.chunked);
  EXPECT_FALSE(reader.eof);
}

TEST(BodyReaderTest, InitializeWithContentLength) {
  httplib::detail::BodyReader reader;
  reader.content_length = 1024;
  reader.chunked = false;

  EXPECT_EQ(1024u, reader.content_length);
  EXPECT_FALSE(reader.chunked);
}

TEST(BodyReaderTest, InitializeAsChunked) {
  httplib::detail::BodyReader reader;
  reader.chunked = true;

  EXPECT_TRUE(reader.chunked);
  EXPECT_EQ(0u, reader.content_length);
}

//------------------------------------------------------------------------------
// Phase 2.3: BodyReader::read() tests
//------------------------------------------------------------------------------

// Mock stream for testing BodyReader
class MockStream : public httplib::Stream {
public:
  explicit MockStream(const std::string &data) : data_(data), pos_(0) {}

  bool is_readable() const override { return pos_ < data_.size(); }
  bool wait_readable() const override { return is_readable(); }
  bool wait_writable() const override { return true; }

  ssize_t read(char *ptr, size_t size) override {
    if (pos_ >= data_.size()) return 0;
    size_t to_read = std::min(size, data_.size() - pos_);
    std::memcpy(ptr, data_.data() + pos_, to_read);
    pos_ += to_read;
    return static_cast<ssize_t>(to_read);
  }

  ssize_t write(const char *, size_t) override { return -1; }

  void get_remote_ip_and_port(std::string &ip, int &port) const override {
    ip = "127.0.0.1";
    port = 0;
  }

  void get_local_ip_and_port(std::string &ip, int &port) const override {
    ip = "127.0.0.1";
    port = 0;
  }

  socket_t socket() const override { return INVALID_SOCKET; }
  time_t duration() const override { return 0; }

private:
  std::string data_;
  size_t pos_;
};

TEST(BodyReaderTest, ReadWithContentLength) {
  std::string body = "Hello, World!";
  MockStream stream(body);

  httplib::detail::BodyReader reader;
  reader.stream = &stream;
  reader.content_length = body.size();
  reader.chunked = false;

  char buf[32];
  auto n = reader.read(buf, sizeof(buf));

  EXPECT_EQ(static_cast<ssize_t>(body.size()), n);
  EXPECT_EQ(body, std::string(buf, static_cast<size_t>(n)));
  EXPECT_EQ(body.size(), reader.bytes_read);
}

TEST(BodyReaderTest, ReadWithContentLengthPartial) {
  std::string body = "Hello, World!";
  MockStream stream(body);

  httplib::detail::BodyReader reader;
  reader.stream = &stream;
  reader.content_length = body.size();
  reader.chunked = false;

  // Read in small chunks
  char buf[5];
  std::string result;

  ssize_t n;
  while ((n = reader.read(buf, sizeof(buf))) > 0) {
    result.append(buf, static_cast<size_t>(n));
  }

  EXPECT_EQ(body, result);
  EXPECT_TRUE(reader.eof);
}

TEST(BodyReaderTest, ReadReturnsZeroAtEOF) {
  std::string body = "Hi";
  MockStream stream(body);

  httplib::detail::BodyReader reader;
  reader.stream = &stream;
  reader.content_length = body.size();
  reader.chunked = false;

  char buf[32];
  reader.read(buf, sizeof(buf)); // Read all

  // Next read should return 0
  auto n = reader.read(buf, sizeof(buf));
  EXPECT_EQ(0, n);
  EXPECT_TRUE(reader.eof);
}

TEST(BodyReaderTest, ReadWithoutStream) {
  httplib::detail::BodyReader reader;
  reader.stream = nullptr;

  char buf[32];
  auto n = reader.read(buf, sizeof(buf));

  EXPECT_EQ(-1, n);
}

// =============================================================================
// StreamHandle v2 Tests (Socket Direct Mode)
// =============================================================================

class StreamHandleV2Test : public ::testing::Test {
protected:
  void SetUp() override {
    mock_stream_ = std::make_unique<MockStream>("Hello from socket!");
  }

  std::unique_ptr<MockStream> mock_stream_;
};

TEST_F(StreamHandleV2Test, SocketDirectModeBasic) {
  // Create StreamHandle with socket direct mode
  httplib::ClientImpl::StreamHandle handle;

  // Set up response (headers only, body will be read from stream)
  handle.response = std::make_unique<httplib::Response>();
  handle.response->status = 200;
  handle.response->set_header("Content-Length", "18");

  // Set up socket direct mode
  handle.stream_ = mock_stream_.get();
  handle.body_reader_.stream = mock_stream_.get();
  handle.body_reader_.content_length = 18;

  EXPECT_TRUE(handle.is_valid());
  EXPECT_TRUE(handle.is_socket_direct_mode());

  // Read from socket
  char buf[32];
  auto n = handle.read(buf, sizeof(buf));
  EXPECT_EQ(18, n);
  EXPECT_EQ(std::string("Hello from socket!"),
            std::string(buf, static_cast<size_t>(n)));

  // EOF
  n = handle.read(buf, sizeof(buf));
  EXPECT_EQ(0, n);
}

TEST_F(StreamHandleV2Test, SocketDirectModeChunkedRead) {
  httplib::ClientImpl::StreamHandle handle;

  handle.response = std::make_unique<httplib::Response>();
  handle.response->status = 200;

  handle.stream_ = mock_stream_.get();
  handle.body_reader_.stream = mock_stream_.get();
  handle.body_reader_.content_length = 18;

  // Read in small chunks
  char buf[5];
  std::string result;

  ssize_t n;
  while ((n = handle.read(buf, sizeof(buf))) > 0) {
    result.append(buf, static_cast<size_t>(n));
  }

  EXPECT_EQ("Hello from socket!", result);
  EXPECT_EQ(0, n); // EOF
}

TEST_F(StreamHandleV2Test, MemoryBufferModeStillWorks) {
  // Existing behavior: read from response->body
  httplib::ClientImpl::StreamHandle handle;

  handle.response = std::make_unique<httplib::Response>();
  handle.response->status = 200;
  handle.response->body = "Memory buffer content";

  // No stream set = memory buffer mode
  EXPECT_TRUE(handle.is_valid());
  EXPECT_FALSE(handle.is_socket_direct_mode());

  char buf[32];
  auto n = handle.read(buf, sizeof(buf));
  EXPECT_EQ(21, n);
  EXPECT_EQ(std::string("Memory buffer content"),
            std::string(buf, static_cast<size_t>(n)));
}

TEST_F(StreamHandleV2Test, ReadAllSocketDirect) {
  httplib::ClientImpl::StreamHandle handle;

  handle.response = std::make_unique<httplib::Response>();
  handle.response->status = 200;

  handle.stream_ = mock_stream_.get();
  handle.body_reader_.stream = mock_stream_.get();
  handle.body_reader_.content_length = 18;

  auto result = handle.read_all();
  EXPECT_EQ("Hello from socket!", result);
}
