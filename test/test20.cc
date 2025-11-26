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
