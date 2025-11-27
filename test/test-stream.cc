// C++20 Streaming API Tests

#include <gtest/gtest.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "../httplib.h"

inline std::string read_all(httplib::ClientImpl::StreamHandle &handle) {
  std::string result;
  char buf[8192];
  ssize_t n;
  while ((n = handle.read(buf, sizeof(buf))) > 0) {
    result.append(buf, static_cast<size_t>(n));
  }
  return result;
}

TEST(StreamHandleTest, Basic) {
  // Test default state
  httplib::ClientImpl::StreamHandle handle;
  EXPECT_FALSE(handle.is_valid());
  EXPECT_EQ(httplib::Error::Success, handle.error);
  EXPECT_EQ(nullptr, handle.response);

  // is_valid returns false when response is null
  handle.error = httplib::Error::Success;
  EXPECT_FALSE(handle.is_valid());

  // is_valid returns false when error is set
  handle.response = std::make_unique<httplib::Response>();
  handle.error = httplib::Error::Connection;
  EXPECT_FALSE(handle.is_valid());

  // is_valid returns true when valid
  handle.error = httplib::Error::Success;
  EXPECT_TRUE(handle.is_valid());
}

class StreamingServerTest : public ::testing::Test {
protected:
  void SetUp() override {
    server_.Get("/hello", [](const httplib::Request &, httplib::Response &res) {
      res.set_content("Hello World!", "text/plain");
    });

    server_.Get("/echo-params",
                [](const httplib::Request &req, httplib::Response &res) {
                  std::string result;
                  for (const auto &p : req.params) {
                    if (!result.empty()) result += "&";
                    result += p.first + "=" + p.second;
                  }
                  res.set_content(result, "text/plain");
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

TEST_F(StreamingServerTest, OpenStream) {
  httplib::Client cli("localhost", 8787);

  // Verify open_stream method exists and returns StreamHandle
  auto handle = cli.open_stream("GET", "/hello");

  // Should be valid for successful request
  EXPECT_TRUE(handle.is_valid());
  EXPECT_EQ(200, handle.response->status);
}

TEST_F(StreamingServerTest, Headers) {
  httplib::Client cli("localhost", 8787);

  auto handle = cli.open_stream("GET", "/hello");

  ASSERT_TRUE(handle.is_valid());
  EXPECT_TRUE(handle.response->has_header("Content-Type"));
  EXPECT_EQ("text/plain", handle.response->get_header_value("Content-Type"));
}

TEST_F(StreamingServerTest, NotFound) {
  httplib::Client cli("localhost", 8787);

  auto handle = cli.open_stream("GET", "/nonexistent");

  // Should still be valid but with 404 status
  EXPECT_TRUE(handle.is_valid());
  EXPECT_EQ(404, handle.response->status);
}

TEST_F(StreamingServerTest, ConnectionError) {
  httplib::Client cli("localhost", 9999);
  auto handle = cli.open_stream("GET", "/hello");
  EXPECT_FALSE(handle.is_valid());
  EXPECT_NE(httplib::Error::Success, handle.error);
}

TEST_F(StreamingServerTest, Read) {
  httplib::Client cli("localhost", 8787);
  auto handle = cli.open_stream("GET", "/hello");
  ASSERT_TRUE(handle.is_valid());

  std::string body;
  char buf[1024];
  ssize_t n;
  while ((n = handle.read(buf, sizeof(buf))) > 0) {
    body.append(buf, static_cast<size_t>(n));
  }
  EXPECT_EQ("Hello World!", body);

  // Further reads should return 0
  n = handle.read(buf, sizeof(buf));
  EXPECT_EQ(0, n);
}

TEST_F(StreamingServerTest, SmallBuffer) {
  httplib::Client cli("localhost", 8787);

  auto handle = cli.open_stream("GET", "/hello");
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

#include "../httplib-stream.h"

inline std::string read_body(httplib::stream::Result &result) {
  std::string body;
  for (auto chunk : result.body()) {
    body.append(chunk);
  }
  return body;
}

TEST_F(StreamingServerTest, GetBasic) {
  httplib::Client cli("localhost", 8787);
  auto result = httplib::stream::Get(cli, "/hello");

  ASSERT_TRUE(result.is_valid());
  EXPECT_EQ(200, result.status());
  EXPECT_TRUE(result.has_header("Content-Type"));
  EXPECT_EQ("text/plain", result.get_header_value("Content-Type"));

  EXPECT_EQ("Hello World!", read_body(result));
  EXPECT_FALSE(result.has_read_error());
}

TEST_F(StreamingServerTest, GetSmallChunks) {
  httplib::Client cli("localhost", 8787);
  auto result = httplib::stream::Get(cli, "/hello");
  ASSERT_TRUE(result.is_valid());

  std::string body;
  size_t chunk_count = 0;
  for (auto chunk : result.body(4)) {
    body.append(chunk);
    chunk_count++;
  }
  EXPECT_EQ("Hello World!", body);
  EXPECT_GT(chunk_count, 1u);
}

TEST_F(StreamingServerTest, GetConnectionError) {
  httplib::Client cli("localhost", 9999);
  auto result = httplib::stream::Get(cli, "/hello");
  EXPECT_FALSE(result.is_valid());
  EXPECT_NE(httplib::Error::Success, result.error());
}

TEST_F(StreamingServerTest, Get404) {
  httplib::Client cli("localhost", 8787);
  auto result = httplib::stream::Get(cli, "/nonexistent");
  EXPECT_TRUE(result.is_valid());
  EXPECT_EQ(404, result.status());
}

TEST_F(StreamingServerTest, GetWithParams) {
  httplib::Client cli("localhost", 8787);
  httplib::Params params = {{"foo", "bar"}, {"baz", "123"}};
  auto result = httplib::stream::Get(cli, "/echo-params", params);

  ASSERT_TRUE(result.is_valid());
  auto body = read_body(result);
  EXPECT_TRUE(body.find("foo=bar") != std::string::npos);
  EXPECT_TRUE(body.find("baz=123") != std::string::npos);
}

TEST_F(StreamingServerTest, GetWithParamsAndHeaders) {
  httplib::Client cli("localhost", 8787);
  httplib::Params params = {{"key", "value"}};
  httplib::Headers headers = {{"X-Custom-Header", "test"}};
  auto result = httplib::stream::Get(cli, "/echo-params", params, headers);

  ASSERT_TRUE(result.is_valid());
  EXPECT_TRUE(read_body(result).find("key=value") != std::string::npos);
}

TEST(GeneratorTest, Basic) {
  auto empty_gen = []() -> httplib::Generator<int> { co_return; }();
  int count = 0;
  for (auto val : empty_gen) {
    (void)val;
    count++;
  }
  EXPECT_EQ(0, count);

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

TEST(ClientConnectionTest, Basic) {
  httplib::ClientConnection conn;
  EXPECT_EQ(INVALID_SOCKET, conn.sock);
  EXPECT_FALSE(conn.is_open());

  conn.sock = 1;
  EXPECT_TRUE(conn.is_open());

  httplib::ClientConnection conn2(std::move(conn));
  EXPECT_EQ(1, conn2.sock);
  EXPECT_EQ(INVALID_SOCKET, conn.sock);

  httplib::ClientConnection conn3;
  conn3 = std::move(conn2);
  EXPECT_EQ(1, conn3.sock);
  EXPECT_EQ(INVALID_SOCKET, conn2.sock);

  conn3.sock = INVALID_SOCKET;
}

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

class ErrorAfterNBytesStream : public httplib::Stream {
public:
  ErrorAfterNBytesStream(const std::string &data, size_t error_after)
      : data_(data), pos_(0), error_after_(error_after) {}
  bool is_readable() const override { return true; }
  bool wait_readable() const override { return true; }
  bool wait_writable() const override { return true; }
  ssize_t read(char *ptr, size_t size) override {
    if (pos_ >= error_after_) { return -1; }
    if (pos_ >= data_.size()) { return 0; }
    size_t to_read =
        std::min(size, std::min(data_.size() - pos_, error_after_ - pos_));
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
  size_t error_after_;
};

TEST(BodyReaderTest, Basic) {
  httplib::detail::BodyReader reader;
  EXPECT_EQ(nullptr, reader.stream);
  EXPECT_EQ(0u, reader.content_length);
  EXPECT_FALSE(reader.chunked);
  EXPECT_FALSE(reader.eof);
  EXPECT_FALSE(reader.has_error());

  std::string body = "Hello, World!";
  MockStream stream(body);
  reader.stream = &stream;
  reader.content_length = body.size();

  char buf[32];
  auto n = reader.read(buf, sizeof(buf));
  EXPECT_EQ(static_cast<ssize_t>(body.size()), n);
  EXPECT_EQ(body, std::string(buf, static_cast<size_t>(n)));

  n = reader.read(buf, sizeof(buf));
  EXPECT_EQ(0, n);
  EXPECT_TRUE(reader.eof);
}

TEST(BodyReaderTest, NoStream) {
  httplib::detail::BodyReader reader;
  reader.stream = nullptr;

  char buf[32];
  auto n = reader.read(buf, sizeof(buf));
  EXPECT_EQ(-1, n);
  EXPECT_TRUE(reader.has_error());
  EXPECT_EQ(httplib::Error::Connection, reader.last_error);
}

TEST(BodyReaderTest, Error) {
  ErrorAfterNBytesStream stream("Hello, World!", 5);
  httplib::detail::BodyReader reader;
  reader.stream = &stream;
  reader.content_length = 13;

  char buf[32];
  auto n1 = reader.read(buf, sizeof(buf));
  EXPECT_EQ(5, n1);
  EXPECT_FALSE(reader.has_error());

  auto n2 = reader.read(buf, sizeof(buf));
  EXPECT_EQ(-1, n2);
  EXPECT_TRUE(reader.has_error());
  EXPECT_EQ(httplib::Error::Read, reader.last_error);
}

class StreamHandleMockTest : public ::testing::Test {
protected:
  void SetUp() override {
    mock_stream_ = std::make_unique<MockStream>("Hello from socket!");
  }
  std::unique_ptr<MockStream> mock_stream_;
};

TEST_F(StreamHandleMockTest, SocketDirect) {
  httplib::ClientImpl::StreamHandle handle;
  handle.response = std::make_unique<httplib::Response>();
  handle.response->status = 200;
  handle.response->set_header("Content-Length", "18");
  handle.stream_ = mock_stream_.get();
  handle.body_reader_.stream = mock_stream_.get();
  handle.body_reader_.content_length = 18;

  EXPECT_TRUE(handle.is_valid());
  EXPECT_TRUE(handle.is_socket_direct_mode());
  EXPECT_FALSE(handle.has_read_error());

  auto result = read_all(handle);
  EXPECT_EQ("Hello from socket!", result);
  EXPECT_FALSE(handle.has_read_error());
}

TEST_F(StreamHandleMockTest, MemoryBuffer) {
  httplib::ClientImpl::StreamHandle handle;
  handle.response = std::make_unique<httplib::Response>();
  handle.response->status = 200;
  handle.response->body = "Memory buffer content";

  EXPECT_TRUE(handle.is_valid());
  EXPECT_FALSE(handle.is_socket_direct_mode());

  char buf[32];
  auto n = handle.read(buf, sizeof(buf));
  EXPECT_EQ(21, n);
  EXPECT_EQ(std::string("Memory buffer content"),
            std::string(buf, static_cast<size_t>(n)));
}

TEST_F(StreamHandleMockTest, Error) {
  auto error_stream =
      std::make_unique<ErrorAfterNBytesStream>("Hello World", 5);

  httplib::ClientImpl::StreamHandle handle;
  handle.response = std::make_unique<httplib::Response>();
  handle.response->status = 200;
  handle.stream_ = error_stream.get();
  handle.body_reader_.stream = error_stream.get();
  handle.body_reader_.content_length = 11;

  char buf[32];
  handle.read(buf, sizeof(buf));
  handle.read(buf, sizeof(buf));

  EXPECT_TRUE(handle.has_read_error());
  EXPECT_EQ(httplib::Error::Read, handle.get_read_error());
}

class OpenStreamTest : public ::testing::Test {
protected:
  void SetUp() override {
    svr_.Get("/hello", [](const httplib::Request &, httplib::Response &res) {
      res.set_content("Hello World!", "text/plain");
    });

    svr_.Get("/large", [](const httplib::Request &, httplib::Response &res) {
      std::string body(10000, 'X');
      res.set_content(body, "text/plain");
    });

    svr_.Get("/chunked", [](const httplib::Request &, httplib::Response &res) {
      res.set_chunked_content_provider(
          "text/plain", [](size_t offset, httplib::DataSink &sink) {
            if (offset < 15) {
              sink.write("chunk", 5);
              return true;
            }
            sink.done();
            return true;
          });
    });

    svr_.Get("/gzip-chunked",
             [](const httplib::Request &, httplib::Response &res) {
               res.set_chunked_content_provider(
                   "text/plain", [](size_t, httplib::DataSink &sink) {
                     sink.os << "This is ";
                     sink.os << "gzip compressed ";
                     sink.os << "chunked data!";
                     sink.done();
                     return true;
                   });
             });

    svr_.Get("/large-compressible",
             [](const httplib::Request &, httplib::Response &res) {
               res.set_chunked_content_provider(
                   "text/plain", [](size_t offset, httplib::DataSink &sink) {
                     const size_t total_size = 100 * 1024;
                     const size_t chunk_size = 8192;

                     if (offset < total_size) {
                       std::string chunk;
                       size_t remaining = total_size - offset;
                       size_t to_write = std::min(chunk_size, remaining);

                       // Repetitive pattern: "Line NNNNNN: Hello World!\n"
                       while (chunk.size() < to_write) {
                         size_t line_num = (offset + chunk.size()) / 28;
                         char line[32];
                         snprintf(line, sizeof(line),
                                  "Line %06zu: Hello World!\n", line_num);
                         chunk += line;
                       }
                       chunk.resize(to_write);
                       sink.write(chunk.data(), chunk.size());
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

TEST_F(OpenStreamTest, Basic) {
  httplib::Client cli("127.0.0.1", 8787);

  auto handle = cli.open_stream("GET", "/hello");

  EXPECT_TRUE(handle.is_valid());
  EXPECT_EQ(200, handle.response->status);
}

TEST_F(OpenStreamTest, SocketDirect) {
  httplib::Client cli("127.0.0.1", 8787);

  auto handle = cli.open_stream("GET", "/hello");

  EXPECT_TRUE(handle.is_valid());
  EXPECT_TRUE(handle.is_socket_direct_mode());
}

TEST_F(OpenStreamTest, ReadBody) {
  httplib::Client cli("127.0.0.1", 8787);

  auto handle = cli.open_stream("GET", "/hello");
  ASSERT_TRUE(handle.is_valid());

  auto body = read_all(handle);
  EXPECT_EQ("Hello World!", body);
}

TEST_F(OpenStreamTest, SmallBuffer) {
  httplib::Client cli("127.0.0.1", 8787);

  auto handle = cli.open_stream("GET", "/hello");
  ASSERT_TRUE(handle.is_valid());

  std::string result;
  char buf[4];
  ssize_t n;
  while ((n = handle.read(buf, sizeof(buf))) > 0) {
    result.append(buf, static_cast<size_t>(n));
  }

  EXPECT_EQ("Hello World!", result);
}

TEST_F(OpenStreamTest, Large) {
  httplib::Client cli("127.0.0.1", 8787);

  auto handle = cli.open_stream("GET", "/large");
  ASSERT_TRUE(handle.is_valid());

  auto body = read_all(handle);
  EXPECT_EQ(10000u, body.size());
  EXPECT_EQ(std::string(10000, 'X'), body);
}

TEST_F(OpenStreamTest, ConnectionError) {
  httplib::Client cli("127.0.0.1", 9999); // Wrong port

  auto handle = cli.open_stream("GET", "/hello");

  EXPECT_FALSE(handle.is_valid());
  EXPECT_NE(httplib::Error::Success, handle.error);
}

TEST_F(OpenStreamTest, Chunked) {
  httplib::Client cli("127.0.0.1", 8787);

  auto handle = cli.open_stream("GET", "/chunked");
  ASSERT_TRUE(handle.is_valid());
  EXPECT_TRUE(handle.body_reader_.chunked);

  auto body = read_all(handle);
  // Server sends "chunk" 3 times
  EXPECT_EQ("chunkchunkchunk", body);
}

TEST_F(OpenStreamTest, ChunkedSmallBuffer) {
  httplib::Client cli("127.0.0.1", 8787);

  auto handle = cli.open_stream("GET", "/chunked");
  ASSERT_TRUE(handle.is_valid());

  std::string result;
  char buf[3]; // Small buffer to force multiple reads
  ssize_t n;
  while ((n = handle.read(buf, sizeof(buf))) > 0) {
    result.append(buf, static_cast<size_t>(n));
  }

  EXPECT_EQ("chunkchunkchunk", result);
}

#ifdef CPPHTTPLIB_ZLIB_SUPPORT
TEST_F(OpenStreamTest, Gzip) {
  httplib::Client cli("127.0.0.1", 8787);
  httplib::Headers headers;
  headers.emplace("Accept-Encoding", "gzip, deflate");

  auto handle = cli.open_stream("GET", "/gzip-chunked", {}, headers);
  ASSERT_TRUE(handle.is_valid());
  EXPECT_EQ("gzip", handle.response->get_header_value("Content-Encoding"));
  EXPECT_TRUE(handle.decompressor_ != nullptr);

  auto body = read_all(handle);
  EXPECT_EQ("This is gzip compressed chunked data!", body);
}

TEST_F(OpenStreamTest, LargeGzip) {
  httplib::Client cli("127.0.0.1", 8787);
  httplib::Headers headers;
  headers.emplace("Accept-Encoding", "gzip, deflate");

  auto handle = cli.open_stream("GET", "/large-compressible", {}, headers);
  ASSERT_TRUE(handle.is_valid());
  EXPECT_EQ("gzip", handle.response->get_header_value("Content-Encoding"));

  auto body = read_all(handle);
  EXPECT_EQ(100 * 1024, body.size());
  EXPECT_TRUE(body.find("Line 000000: Hello World!") != std::string::npos);
}

TEST_F(OpenStreamTest, NoCompression) {
  httplib::Client cli("127.0.0.1", 8787);
  auto handle = cli.open_stream("GET", "/gzip-chunked");
  ASSERT_TRUE(handle.is_valid());
  EXPECT_TRUE(handle.response->get_header_value("Content-Encoding").empty());
  EXPECT_TRUE(handle.decompressor_ == nullptr);

  auto body = read_all(handle);
  EXPECT_EQ("This is gzip compressed chunked data!", body);
}
#endif

#ifdef CPPHTTPLIB_BROTLI_SUPPORT
TEST_F(OpenStreamTest, Brotli) {
  httplib::Client cli("127.0.0.1", 8787);
  httplib::Headers headers;
  headers.emplace("Accept-Encoding", "br");

  auto handle = cli.open_stream("GET", "/large-compressible", {}, headers);
  ASSERT_TRUE(handle.is_valid());
  EXPECT_EQ("br", handle.response->get_header_value("Content-Encoding"));

  auto body = read_all(handle);
  EXPECT_EQ(100 * 1024, body.size());
}
#endif

#ifdef CPPHTTPLIB_ZSTD_SUPPORT
TEST_F(OpenStreamTest, Zstd) {
  httplib::Client cli("127.0.0.1", 8787);
  httplib::Headers headers;
  headers.emplace("Accept-Encoding", "zstd");

  auto handle = cli.open_stream("GET", "/large-compressible", {}, headers);
  ASSERT_TRUE(handle.is_valid());
  EXPECT_EQ("zstd", handle.response->get_header_value("Content-Encoding"));

  auto body = read_all(handle);
  EXPECT_EQ(100 * 1024, body.size());
}
#endif

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
class SSLOpenStreamTest : public ::testing::Test {
protected:
  SSLOpenStreamTest() : svr_("cert.pem", "key.pem") {}

  void SetUp() override {
    svr_.Get("/hello", [](const httplib::Request &, httplib::Response &res) {
      res.set_content("Hello SSL World!", "text/plain");
    });

    svr_.Get("/chunked", [](const httplib::Request &, httplib::Response &res) {
      res.set_chunked_content_provider(
          "text/plain", [](size_t offset, httplib::DataSink &sink) {
            if (offset < 15) {
              sink.write("chunk", 5);
              return true;
            }
            sink.done();
            return true;
          });
    });

    thread_ = std::thread([this]() { svr_.listen("127.0.0.1", 8788); });
    svr_.wait_until_ready();
  }

  void TearDown() override {
    svr_.stop();
    if (thread_.joinable()) { thread_.join(); }
  }

  httplib::SSLServer svr_;
  std::thread thread_;
};

TEST_F(SSLOpenStreamTest, Basic) {
  httplib::SSLClient cli("127.0.0.1", 8788);
  cli.enable_server_certificate_verification(false);

  auto handle = cli.open_stream("GET", "/hello");

  ASSERT_TRUE(handle.is_valid()) << "Error: " << static_cast<int>(handle.error);
  EXPECT_EQ(200, handle.response->status);
  EXPECT_TRUE(handle.is_socket_direct_mode());

  auto body = read_all(handle);
  EXPECT_EQ("Hello SSL World!", body);
}

TEST_F(SSLOpenStreamTest, Chunked) {
  httplib::SSLClient cli("127.0.0.1", 8788);
  cli.enable_server_certificate_verification(false);

  auto handle = cli.open_stream("GET", "/chunked");

  ASSERT_TRUE(handle.is_valid()) << "Error: " << static_cast<int>(handle.error);
  EXPECT_TRUE(handle.body_reader_.chunked);

  auto body = read_all(handle);
  EXPECT_EQ("chunkchunkchunk", body);
}
#endif

//------------------------------------------------------------------------------
// POST/PUT/PATCH Streaming Response Tests
//------------------------------------------------------------------------------

class PostStreamingTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Echo endpoint: returns the request body
    server_.Post(
        "/echo", [](const httplib::Request &req, httplib::Response &res) {
          res.set_content(req.body, req.get_header_value("Content-Type"));
        });

    // Echo with streaming response (chunked)
    server_.Post("/echo-chunked", [](const httplib::Request &req,
                                     httplib::Response &res) {
      std::string body = req.body;
      res.set_chunked_content_provider(
          "text/plain", [body](size_t offset, httplib::DataSink &sink) {
            if (offset < body.size()) {
              size_t chunk_size = std::min(size_t(10), body.size() - offset);
              sink.write(body.data() + offset, chunk_size);
              return true;
            }
            sink.done();
            return true;
          });
    });

    // Large response endpoint
    server_.Post("/large-response",
                 [](const httplib::Request &, httplib::Response &res) {
                   // Return 100KB of data
                   std::string large_body(100 * 1024, 'X');
                   res.set_content(large_body, "application/octet-stream");
                 });

    // Echo with headers
    server_.Post("/echo-headers",
                 [](const httplib::Request &req, httplib::Response &res) {
                   std::string result;
                   for (const auto &h : req.headers) {
                     result += h.first + ": " + h.second + "\n";
                   }
                   res.set_content(result, "text/plain");
                 });

    // PUT endpoint
    server_.Put("/put-echo",
                [](const httplib::Request &req, httplib::Response &res) {
                  res.set_content("PUT:" + req.body, "text/plain");
                });

    // PATCH endpoint
    server_.Patch("/patch-echo",
                  [](const httplib::Request &req, httplib::Response &res) {
                    res.set_content("PATCH:" + req.body, "text/plain");
                  });

    // Echo query params and body
    server_.Post("/echo-params",
                 [](const httplib::Request &req, httplib::Response &res) {
                   std::string result = "params:";
                   for (const auto &p : req.params) {
                     result += p.first + "=" + p.second + ";";
                   }
                   result += " body:" + req.body;
                   res.set_content(result, "text/plain");
                 });

    // Start server
    server_thread_ =
        std::thread([this]() { server_.listen("localhost", 8799); });

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

// Basic POST streaming test
TEST_F(PostStreamingTest, Basic) {
  httplib::Client cli("localhost", 8799);

  auto result = httplib::stream::Post(cli, "/echo", "Hello POST", "text/plain");

  ASSERT_TRUE(result.is_valid());
  EXPECT_EQ(200, result.status());
  EXPECT_EQ("text/plain", result.get_header_value("Content-Type"));

  EXPECT_EQ("Hello POST", read_body(result));
}

// POST with custom headers
TEST_F(PostStreamingTest, WithHeaders) {
  httplib::Client cli("localhost", 8799);

  httplib::Headers headers = {{"X-Custom-Header", "custom-value"}};
  auto result = httplib::stream::Post(cli, "/echo-headers", headers, "body",
                                      "text/plain");

  ASSERT_TRUE(result.is_valid());
  EXPECT_EQ(200, result.status());

  EXPECT_TRUE(read_body(result).find("X-Custom-Header: custom-value") !=
              std::string::npos);
}

// POST with chunked response streaming
TEST_F(PostStreamingTest, Chunked) {
  httplib::Client cli("localhost", 8799);

  auto result = httplib::stream::Post(cli, "/echo-chunked",
                                      "Hello Chunked World", "text/plain");

  ASSERT_TRUE(result.is_valid());
  EXPECT_EQ(200, result.status());

  EXPECT_EQ("Hello Chunked World", read_body(result));
}

// POST with large response
TEST_F(PostStreamingTest, Large) {
  httplib::Client cli("localhost", 8799);

  auto result =
      httplib::stream::Post(cli, "/large-response", "trigger", "text/plain");

  ASSERT_TRUE(result.is_valid());
  EXPECT_EQ(200, result.status());

  size_t total_size = 0;
  for (auto chunk : result.body()) {
    total_size += chunk.size();
  }
  EXPECT_EQ(100u * 1024u, total_size);
}

// PUT/PATCH streaming tests (minimal - same template as POST)
TEST_F(PostStreamingTest, PutAndPatch) {
  httplib::Client cli("localhost", 8799);

  // PUT test
  auto put_result =
      httplib::stream::Put(cli, "/put-echo", "Hello PUT", "text/plain");
  ASSERT_TRUE(put_result.is_valid());
  EXPECT_EQ(200, put_result.status());
  EXPECT_EQ("PUT:Hello PUT", read_body(put_result));

  // PATCH test
  auto patch_result =
      httplib::stream::Patch(cli, "/patch-echo", "Hello PATCH", "text/plain");
  ASSERT_TRUE(patch_result.is_valid());
  EXPECT_EQ(200, patch_result.status());
  EXPECT_EQ("PATCH:Hello PATCH", read_body(patch_result));
}

// POST with empty body
TEST_F(PostStreamingTest, EmptyBody) {
  httplib::Client cli("localhost", 8799);

  auto result = httplib::stream::Post(cli, "/echo", "", "text/plain");

  ASSERT_TRUE(result.is_valid());
  EXPECT_EQ(200, result.status());

  EXPECT_TRUE(read_body(result).empty());
}

// POST with JSON content type
TEST_F(PostStreamingTest, Json) {
  httplib::Client cli("localhost", 8799);

  auto result = httplib::stream::Post(cli, "/echo", R"({"key":"value"})",
                                      "application/json");

  ASSERT_TRUE(result.is_valid());
  EXPECT_EQ(200, result.status());
  EXPECT_EQ("application/json", result.get_header_value("Content-Type"));

  EXPECT_EQ(R"({"key":"value"})", read_body(result));
}

// stream::Result validity check
TEST_F(PostStreamingTest, Validity) {
  httplib::Client cli("localhost", 8799);

  auto result = httplib::stream::Post(cli, "/echo", "test", "text/plain");

  EXPECT_TRUE(result.is_valid());
  EXPECT_TRUE(static_cast<bool>(result));
  EXPECT_EQ(httplib::Error::Success, result.error());
}

// POST with Params
TEST_F(PostStreamingTest, WithParams) {
  httplib::Client cli("localhost", 8799);

  httplib::Params params = {{"key1", "value1"}, {"key2", "value2"}};
  auto result = httplib::stream::Post(cli, "/echo-params", params, "body data",
                                      "text/plain");

  ASSERT_TRUE(result.is_valid());
  EXPECT_EQ(200, result.status());

  auto body = read_body(result);
  EXPECT_TRUE(body.find("key1=value1") != std::string::npos);
  EXPECT_TRUE(body.find("key2=value2") != std::string::npos);
  EXPECT_TRUE(body.find("body:body data") != std::string::npos);
}

// POST with Params and Headers
TEST_F(PostStreamingTest, WithParamsAndHeaders) {
  httplib::Client cli("localhost", 8799);

  httplib::Params params = {{"id", "123"}};
  httplib::Headers headers = {{"X-Custom", "test"}};
  auto result = httplib::stream::Post(cli, "/echo-params", params, headers,
                                      "json body", "application/json");

  ASSERT_TRUE(result.is_valid());
  EXPECT_EQ(200, result.status());

  EXPECT_TRUE(read_body(result).find("id=123") != std::string::npos);
}

// POST to non-existent endpoint
TEST_F(PostStreamingTest, NotFound) {
  httplib::Client cli("localhost", 8799);

  auto result =
      httplib::stream::Post(cli, "/nonexistent", "test", "text/plain");

  EXPECT_TRUE(result.is_valid());  // Connection succeeded
  EXPECT_EQ(404, result.status()); // But endpoint not found
}

// Connection error
TEST_F(PostStreamingTest, ConnectionError) {
  httplib::Client cli("localhost", 9999); // No server

  auto result = httplib::stream::Post(cli, "/echo", "test", "text/plain");

  EXPECT_FALSE(result.is_valid());
  EXPECT_NE(httplib::Error::Success, result.error());
}

//------------------------------------------------------------------------------
// SSL POST Streaming Tests
//------------------------------------------------------------------------------
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT

class SSLPostStreamingTest : public ::testing::Test {
protected:
  void SetUp() override {
    server_.Post(
        "/echo", [](const httplib::Request &req, httplib::Response &res) {
          res.set_content(req.body, req.get_header_value("Content-Type"));
        });

    // GET endpoint for stream::Get template test
    server_.Get("/hello", [](const httplib::Request &, httplib::Response &res) {
      res.set_content("Hello SSL World!", "text/plain");
    });

    server_.Put("/put-echo",
                [](const httplib::Request &req, httplib::Response &res) {
                  res.set_content("PUT:" + req.body, "text/plain");
                });

    server_.Patch("/patch-echo",
                  [](const httplib::Request &req, httplib::Response &res) {
                    res.set_content("PATCH:" + req.body, "text/plain");
                  });

    server_.Post("/chunked-response", [](const httplib::Request &req,
                                         httplib::Response &res) {
      std::string body = req.body;
      res.set_chunked_content_provider(
          "text/plain", [body](size_t offset, httplib::DataSink &sink) {
            if (offset < body.size()) {
              sink.write(body.data() + offset, body.size() - offset);
            }
            sink.done();
            return true;
          });
    });

    server_thread_ =
        std::thread([this]() { server_.listen("127.0.0.1", 8801); });

    while (!server_.is_running()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  void TearDown() override {
    server_.stop();
    if (server_thread_.joinable()) { server_thread_.join(); }
  }

  httplib::SSLServer server_{"cert.pem", "key.pem"};
  std::thread server_thread_;
};

TEST_F(SSLPostStreamingTest, Basic) {
  httplib::SSLClient cli("127.0.0.1", 8801);
  cli.enable_server_certificate_verification(false);

  auto handle =
      cli.open_stream("POST", "/echo", {}, {}, "Hello SSL POST", "text/plain");

  ASSERT_TRUE(handle.is_valid()) << "Error: " << static_cast<int>(handle.error);
  EXPECT_EQ(200, handle.response->status);

  auto body = read_all(handle);
  EXPECT_EQ("Hello SSL POST", body);
}

TEST_F(SSLPostStreamingTest, Chunked) {
  httplib::SSLClient cli("127.0.0.1", 8801);
  cli.enable_server_certificate_verification(false);

  auto handle = cli.open_stream("POST", "/chunked-response", {}, {},
                                "Chunked SSL Data", "text/plain");

  ASSERT_TRUE(handle.is_valid());
  EXPECT_EQ(200, handle.response->status);

  auto body = read_all(handle);
  EXPECT_EQ("Chunked SSL Data", body);
}

// Test stream::Get with SSLClient (template version)
TEST_F(SSLPostStreamingTest, Get) {
  httplib::SSLClient cli("127.0.0.1", 8801);
  cli.enable_server_certificate_verification(false);

  // Use stream::Get with SSLClient
  auto result = httplib::stream::Get(cli, "/hello");

  ASSERT_TRUE(result.is_valid());
  EXPECT_EQ(200, result.status());

  EXPECT_EQ("Hello SSL World!", read_body(result));
}

// Test stream::Post with SSLClient (template version)
TEST_F(SSLPostStreamingTest, Post) {
  httplib::SSLClient cli("127.0.0.1", 8801);
  cli.enable_server_certificate_verification(false);

  auto result = httplib::stream::Post(cli, "/echo", "Test Body", "text/plain");

  ASSERT_TRUE(result.is_valid());
  EXPECT_EQ(200, result.status());

  EXPECT_EQ("Test Body", read_body(result));
}

#endif // CPPHTTPLIB_OPENSSL_SUPPORT

//------------------------------------------------------------------------------
// DELETE/HEAD/OPTIONS Streaming Response Tests
//------------------------------------------------------------------------------

class OtherMethodsStreamingTest : public ::testing::Test {
protected:
  void SetUp() override {
    // DELETE endpoint
    server_.Delete("/resource",
                   [](const httplib::Request &, httplib::Response &res) {
                     res.set_content("Deleted", "text/plain");
                   });

    // DELETE with body endpoint
    server_.Delete("/resource-with-body",
                   [](const httplib::Request &req, httplib::Response &res) {
                     res.set_content("Deleted:" + req.body, "text/plain");
                   });

    // DELETE with params endpoint
    server_.Delete("/resource-params",
                   [](const httplib::Request &req, httplib::Response &res) {
                     std::string result = "Deleted:";
                     for (const auto &p : req.params) {
                       result += p.first + "=" + p.second + ";";
                     }
                     res.set_content(result, "text/plain");
                   });

    // HEAD endpoint (body is ignored in response)
    server_.Get(
        "/head-test", [](const httplib::Request &, httplib::Response &res) {
          res.set_content("This body will be ignored for HEAD", "text/plain");
        });

    // OPTIONS endpoint
    server_.Options(
        "/options-test", [](const httplib::Request &, httplib::Response &res) {
          res.set_header("Allow", "GET, POST, PUT, DELETE, OPTIONS");
          res.set_content("", "text/plain");
        });

    // Start server
    server_thread_ =
        std::thread([this]() { server_.listen("localhost", 8802); });

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

// DELETE tests
TEST_F(OtherMethodsStreamingTest, Delete) {
  httplib::Client cli("localhost", 8802);

  auto result = httplib::stream::Delete(cli, "/resource");

  ASSERT_TRUE(result.is_valid());
  EXPECT_EQ(200, result.status());

  EXPECT_EQ("Deleted", read_body(result));
}

TEST_F(OtherMethodsStreamingTest, DeleteHeaders) {
  httplib::Client cli("localhost", 8802);

  httplib::Headers headers = {{"X-Custom", "Header"}};
  auto result = httplib::stream::Delete(cli, "/resource", headers);

  ASSERT_TRUE(result.is_valid());
  EXPECT_EQ(200, result.status());
}

TEST_F(OtherMethodsStreamingTest, DeleteBody) {
  httplib::Client cli("localhost", 8802);

  auto result = httplib::stream::Delete(cli, "/resource-with-body",
                                        "delete-data", "text/plain");

  ASSERT_TRUE(result.is_valid());
  EXPECT_EQ(200, result.status());

  EXPECT_EQ("Deleted:delete-data", read_body(result));
}

TEST_F(OtherMethodsStreamingTest, DeleteParams) {
  httplib::Client cli("localhost", 8802);

  httplib::Params params = {{"id", "123"}};
  auto result = httplib::stream::Delete(cli, "/resource-params", params);

  ASSERT_TRUE(result.is_valid());
  EXPECT_EQ(200, result.status());

  EXPECT_EQ("Deleted:id=123;", read_body(result));
}

TEST_F(OtherMethodsStreamingTest, HeadAndOptions) {
  httplib::Client cli("localhost", 8802);

  // HEAD
  auto head_result = httplib::stream::Head(cli, "/head-test");
  ASSERT_TRUE(head_result.is_valid());
  EXPECT_EQ(200, head_result.status());
  EXPECT_FALSE(head_result.get_header_value("Content-Length").empty());

  // HEAD with params
  httplib::Params params = {{"key", "value"}};
  auto head_params = httplib::stream::Head(cli, "/head-test", params);
  ASSERT_TRUE(head_params.is_valid());

  // OPTIONS
  auto options_result = httplib::stream::Options(cli, "/options-test");
  ASSERT_TRUE(options_result.is_valid());
  EXPECT_EQ(200, options_result.status());
  EXPECT_EQ("GET, POST, PUT, DELETE, OPTIONS",
            options_result.get_header_value("Allow"));
}
