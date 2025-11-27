//==============================================================================
// C++20 stream::* API Tests (httplib-stream.h)
// These tests require C++20 and use the Generator-based streaming API.
// For open_stream() tests, see test.cc.
//==============================================================================

#include <gtest/gtest.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "../httplib-stream.h"
#include "../httplib.h"

inline std::string read_body(httplib::stream::Result &&result) {
  std::string body;
  for (auto chunk : result.body())
    body.append(chunk);
  return body;
}

inline std::string read_body(httplib::stream::Result &result) {
  std::string body;
  for (auto chunk : result.body())
    body.append(chunk);
  return body;
}

TEST(GeneratorTest, Basic) {
  auto gen = []() -> httplib::Generator<int> {
    co_yield 1;
    co_yield 2;
    co_yield 3;
  }();
  std::vector<int> values;
  for (auto val : gen)
    values.push_back(val);
  EXPECT_EQ(3u, values.size());
}

TEST(ClientConnectionTest, Basic) {
  httplib::ClientConnection conn;
  EXPECT_FALSE(conn.is_open());
  conn.sock = 1;
  EXPECT_TRUE(conn.is_open());
  httplib::ClientConnection conn2(std::move(conn));
  EXPECT_EQ(INVALID_SOCKET, conn.sock);
  conn2.sock = INVALID_SOCKET;
}

// Unified test server for all stream::* tests
class StreamApiTest : public ::testing::Test {
protected:
  void SetUp() override {
    svr_.Get("/hello", [](const httplib::Request &, httplib::Response &res) {
      res.set_content("Hello World!", "text/plain");
    });
    svr_.Get("/echo-params",
             [](const httplib::Request &req, httplib::Response &res) {
               std::string r;
               for (const auto &p : req.params) {
                 if (!r.empty()) r += "&";
                 r += p.first + "=" + p.second;
               }
               res.set_content(r, "text/plain");
             });
    svr_.Post("/echo", [](const httplib::Request &req, httplib::Response &res) {
      res.set_content(req.body, req.get_header_value("Content-Type"));
    });
    svr_.Post("/echo-headers",
              [](const httplib::Request &req, httplib::Response &res) {
                std::string r;
                for (const auto &h : req.headers)
                  r += h.first + ": " + h.second + "\n";
                res.set_content(r, "text/plain");
              });
    svr_.Post("/echo-params",
              [](const httplib::Request &req, httplib::Response &res) {
                std::string r = "params:";
                for (const auto &p : req.params)
                  r += p.first + "=" + p.second + ";";
                res.set_content(r + " body:" + req.body, "text/plain");
              });
    svr_.Post("/large", [](const httplib::Request &, httplib::Response &res) {
      res.set_content(std::string(100 * 1024, 'X'), "application/octet-stream");
    });
    svr_.Put("/echo", [](const httplib::Request &req, httplib::Response &res) {
      res.set_content("PUT:" + req.body, "text/plain");
    });
    svr_.Patch("/echo",
               [](const httplib::Request &req, httplib::Response &res) {
                 res.set_content("PATCH:" + req.body, "text/plain");
               });
    svr_.Delete(
        "/resource", [](const httplib::Request &req, httplib::Response &res) {
          res.set_content(req.body.empty() ? "Deleted" : "Deleted:" + req.body,
                          "text/plain");
        });
    svr_.Get("/head-test",
             [](const httplib::Request &, httplib::Response &res) {
               res.set_content("body for HEAD", "text/plain");
             });
    svr_.Options("/options",
                 [](const httplib::Request &, httplib::Response &res) {
                   res.set_header("Allow", "GET, POST, PUT, DELETE, OPTIONS");
                 });
    thread_ = std::thread([this]() { svr_.listen("localhost", 8790); });
    svr_.wait_until_ready();
  }
  void TearDown() override {
    svr_.stop();
    if (thread_.joinable()) thread_.join();
  }
  httplib::Server svr_;
  std::thread thread_;
};

// stream::Get tests
TEST_F(StreamApiTest, GetBasic) {
  httplib::Client cli("localhost", 8790);
  auto result = httplib::stream::Get(cli, "/hello");
  ASSERT_TRUE(result.is_valid());
  EXPECT_EQ(200, result.status());
  EXPECT_EQ("Hello World!", read_body(result));
}

TEST_F(StreamApiTest, GetWithParams) {
  httplib::Client cli("localhost", 8790);
  httplib::Params params{{"foo", "bar"}};
  auto result = httplib::stream::Get(cli, "/echo-params", params);
  ASSERT_TRUE(result.is_valid());
  EXPECT_TRUE(read_body(result).find("foo=bar") != std::string::npos);
}

TEST_F(StreamApiTest, GetConnectionError) {
  httplib::Client cli("localhost", 9999);
  EXPECT_FALSE(httplib::stream::Get(cli, "/hello").is_valid());
}

TEST_F(StreamApiTest, Get404) {
  httplib::Client cli("localhost", 8790);
  auto result = httplib::stream::Get(cli, "/nonexistent");
  EXPECT_TRUE(result.is_valid());
  EXPECT_EQ(404, result.status());
}

// stream::Post tests
TEST_F(StreamApiTest, PostBasic) {
  httplib::Client cli("localhost", 8790);
  auto result = httplib::stream::Post(cli, "/echo", R"({"key":"value"})",
                                      "application/json");
  ASSERT_TRUE(result.is_valid());
  EXPECT_EQ("application/json", result.get_header_value("Content-Type"));
  EXPECT_EQ(R"({"key":"value"})", read_body(result));
}

TEST_F(StreamApiTest, PostWithHeaders) {
  httplib::Client cli("localhost", 8790);
  httplib::Headers headers{{"X-Custom", "value"}};
  auto result = httplib::stream::Post(cli, "/echo-headers", headers, "body",
                                      "text/plain");
  EXPECT_TRUE(read_body(result).find("X-Custom: value") != std::string::npos);
}

TEST_F(StreamApiTest, PostWithParams) {
  httplib::Client cli("localhost", 8790);
  httplib::Params params{{"k", "v"}};
  auto result =
      httplib::stream::Post(cli, "/echo-params", params, "data", "text/plain");
  auto body = read_body(result);
  EXPECT_TRUE(body.find("k=v") != std::string::npos);
  EXPECT_TRUE(body.find("body:data") != std::string::npos);
}

TEST_F(StreamApiTest, PostLarge) {
  httplib::Client cli("localhost", 8790);
  auto result = httplib::stream::Post(cli, "/large", "", "text/plain");
  size_t total = 0;
  for (auto chunk : result.body())
    total += chunk.size();
  EXPECT_EQ(100u * 1024u, total);
}

// stream::Put/Patch tests
TEST_F(StreamApiTest, PutAndPatch) {
  httplib::Client cli("localhost", 8790);
  auto put = httplib::stream::Put(cli, "/echo", "test", "text/plain");
  EXPECT_EQ("PUT:test", read_body(put));
  auto patch = httplib::stream::Patch(cli, "/echo", "test", "text/plain");
  EXPECT_EQ("PATCH:test", read_body(patch));
}

// stream::Delete tests
TEST_F(StreamApiTest, Delete) {
  httplib::Client cli("localhost", 8790);
  auto del1 = httplib::stream::Delete(cli, "/resource");
  EXPECT_EQ("Deleted", read_body(del1));
  auto del2 = httplib::stream::Delete(cli, "/resource", "data", "text/plain");
  EXPECT_EQ("Deleted:data", read_body(del2));
}

// stream::Head/Options tests
TEST_F(StreamApiTest, HeadAndOptions) {
  httplib::Client cli("localhost", 8790);
  auto head = httplib::stream::Head(cli, "/head-test");
  EXPECT_TRUE(head.is_valid());
  EXPECT_FALSE(head.get_header_value("Content-Length").empty());

  auto opts = httplib::stream::Options(cli, "/options");
  EXPECT_EQ("GET, POST, PUT, DELETE, OPTIONS", opts.get_header_value("Allow"));
}

// SSL stream::* tests
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
class SSLStreamApiTest : public ::testing::Test {
protected:
  void SetUp() override {
    svr_.Get("/hello", [](const httplib::Request &, httplib::Response &res) {
      res.set_content("Hello SSL!", "text/plain");
    });
    svr_.Post("/echo", [](const httplib::Request &req, httplib::Response &res) {
      res.set_content(req.body, "text/plain");
    });
    thread_ = std::thread([this]() { svr_.listen("127.0.0.1", 8803); });
    svr_.wait_until_ready();
  }
  void TearDown() override {
    svr_.stop();
    if (thread_.joinable()) thread_.join();
  }
  httplib::SSLServer svr_{"cert.pem", "key.pem"};
  std::thread thread_;
};

TEST_F(SSLStreamApiTest, GetAndPost) {
  httplib::SSLClient cli("127.0.0.1", 8803);
  cli.enable_server_certificate_verification(false);
  auto get = httplib::stream::Get(cli, "/hello");
  EXPECT_EQ("Hello SSL!", read_body(get));
  auto post = httplib::stream::Post(cli, "/echo", "test", "text/plain");
  EXPECT_EQ("test", read_body(post));
}
#endif
