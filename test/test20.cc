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
