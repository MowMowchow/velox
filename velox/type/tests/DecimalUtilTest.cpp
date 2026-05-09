/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Tests for the to_chars __int128 shim that backs DecimalUtil::castToString
 * on libstdc++ (which lacks std::to_chars(__int128)).
 *
 * Coverage targets the canonical-correctness contract per the
 * delphi/to_chars_int128_shim spec:
 *   1. Round-trip: serialize then deserialize equals the input.
 *   2. Zero: writes "0", returns ptr=buf+1, ec=success.
 *   3. INT128_MIN: handles the negation overflow edge case.
 *   4. INT128_MAX: signed 128-bit upper bound.
 *   5. Buffer overflow: returns value_too_large and ptr==last.
 *   6. UINT128_MAX: unsigned 128-bit upper bound.
 */

#include "velox/external/charconv_int128/charconv_int128.h"

#include <cstring>
#include <limits>
#include <string>
#include <system_error>

#include <gtest/gtest.h>

namespace facebook::velox::charconv::test {
namespace {

// Type aliases used throughout -- match velox/type/HugeInt.h conventions.
using int128_t = __int128;
using uint128_t = unsigned __int128;

// Helper: parse a (possibly signed) decimal string back into an int128_t.
// Used only in the round-trip test -- exists because std::from_chars also
// lacks __int128 on libstdc++, so we hand-roll the inverse.
int128_t parseInt128Decimal(const std::string& s) {
  size_t i = 0;
  bool negative = false;
  if (!s.empty() && s[0] == '-') {
    negative = true;
    i = 1;
  }
  uint128_t mag = 0;
  for (; i < s.size(); ++i) {
    mag = mag * 10 + static_cast<uint128_t>(s[i] - '0');
  }
  if (negative) {
    return -static_cast<int128_t>(mag);
  }
  return static_cast<int128_t>(mag);
}

TEST(ToCharsInt128, RoundTripSmallPositive) {
  char buf[64];
  const int128_t input = 12345;
  auto res = to_chars_int128(buf, buf + sizeof(buf), input);
  ASSERT_EQ(res.ec, std::errc{}) << "expected success, got "
                                 << std::make_error_code(res.ec).message();
  const std::string written(buf, res.ptr - buf);
  EXPECT_EQ(written, "12345");
  EXPECT_EQ(parseInt128Decimal(written), input);
}

TEST(ToCharsInt128, Zero) {
  char buf[64];
  std::memset(buf, '\xFF', sizeof(buf));  // poison
  auto res = to_chars_int128(buf, buf + 1, int128_t{0});
  ASSERT_EQ(res.ec, std::errc{});
  EXPECT_EQ(res.ptr, buf + 1);
  EXPECT_EQ(buf[0], '0');
}

TEST(ToCharsInt128, ZeroUnsigned) {
  char buf[64];
  std::memset(buf, '\xFF', sizeof(buf));
  auto res = to_chars_int128(buf, buf + 1, uint128_t{0});
  ASSERT_EQ(res.ec, std::errc{});
  EXPECT_EQ(res.ptr, buf + 1);
  EXPECT_EQ(buf[0], '0');
}

TEST(ToCharsInt128, Int128Min) {
  // INT128_MIN = -(2^127) = -170141183460469231731687303715884105728
  char buf[64];
  const int128_t input = std::numeric_limits<int128_t>::min();
  auto res = to_chars_int128(buf, buf + sizeof(buf), input);
  ASSERT_EQ(res.ec, std::errc{});
  const std::string written(buf, res.ptr - buf);
  EXPECT_EQ(written, "-170141183460469231731687303715884105728");
  EXPECT_EQ(parseInt128Decimal(written), input);
}

TEST(ToCharsInt128, Int128Max) {
  // INT128_MAX = 2^127 - 1 = 170141183460469231731687303715884105727
  char buf[64];
  const int128_t input = std::numeric_limits<int128_t>::max();
  auto res = to_chars_int128(buf, buf + sizeof(buf), input);
  ASSERT_EQ(res.ec, std::errc{});
  const std::string written(buf, res.ptr - buf);
  EXPECT_EQ(written, "170141183460469231731687303715884105727");
  EXPECT_EQ(parseInt128Decimal(written), input);
}

TEST(ToCharsInt128, BufferOverflow) {
  // 12345678 needs 8 chars but buffer is only 5.
  char buf[5];
  std::memset(buf, '\xFF', sizeof(buf));
  auto res = to_chars_int128(buf, buf + 5, int128_t{12345678});
  EXPECT_EQ(res.ec, std::errc::value_too_large);
  EXPECT_EQ(res.ptr, buf + 5);
}

TEST(ToCharsInt128, BufferOverflowExact) {
  // Boundary: 12345 needs exactly 5 chars; buffer of 5 should succeed.
  char buf[5];
  auto res = to_chars_int128(buf, buf + 5, int128_t{12345});
  ASSERT_EQ(res.ec, std::errc{});
  EXPECT_EQ(res.ptr, buf + 5);
  EXPECT_EQ(std::string(buf, 5), "12345");
}

TEST(ToCharsInt128, BufferOverflowOffByOne) {
  // 12345 needs 5 chars; buffer of 4 should fail.
  char buf[4];
  auto res = to_chars_int128(buf, buf + 4, int128_t{12345});
  EXPECT_EQ(res.ec, std::errc::value_too_large);
  EXPECT_EQ(res.ptr, buf + 4);
}

TEST(ToCharsInt128, NegativeOneCharBuffer) {
  // Negative input requires at least 2 chars ('-' + digit).
  // A 1-char buffer cannot fit even '-' alone with a digit.
  char buf[1];
  auto res = to_chars_int128(buf, buf + 1, int128_t{-5});
  EXPECT_EQ(res.ec, std::errc::value_too_large);
}

TEST(ToCharsInt128, EmptyBufferZero) {
  // Edge case: zero needs 1 byte; empty buffer should report overflow.
  char dummy = '\xFF';
  auto res = to_chars_int128(&dummy, &dummy, int128_t{0});
  EXPECT_EQ(res.ec, std::errc::value_too_large);
  EXPECT_EQ(res.ptr, &dummy);
}

TEST(ToCharsInt128, Uint128Max) {
  // UINT128_MAX = 2^128 - 1 = 340282366920938463463374607431768211455
  char buf[64];
  const uint128_t input = std::numeric_limits<uint128_t>::max();
  auto res = to_chars_int128(buf, buf + sizeof(buf), input);
  ASSERT_EQ(res.ec, std::errc{});
  const std::string written(buf, res.ptr - buf);
  EXPECT_EQ(written, "340282366920938463463374607431768211455");
}

TEST(ToCharsInt128, NegativeSmall) {
  char buf[64];
  auto res = to_chars_int128(buf, buf + sizeof(buf), int128_t{-12345});
  ASSERT_EQ(res.ec, std::errc{});
  EXPECT_EQ(std::string(buf, res.ptr - buf), "-12345");
}

TEST(ToCharsInt128, GenericForwardingInt32) {
  // The SFINAE'd template overload should forward 32-bit ints to
  // std::to_chars verbatim. This is what allows DecimalUtil's int32_t
  // callsites (lines 360, 405) to use the unified shim entry point.
  char buf[16];
  auto res = to_chars_int128(buf, buf + sizeof(buf), int32_t{-42});
  ASSERT_EQ(res.ec, std::errc{});
  EXPECT_EQ(std::string(buf, res.ptr - buf), "-42");
}

TEST(ToCharsInt128, GenericForwardingInt64) {
  char buf[32];
  auto res =
      to_chars_int128(buf, buf + sizeof(buf), int64_t{9223372036854775807LL});
  ASSERT_EQ(res.ec, std::errc{});
  EXPECT_EQ(std::string(buf, res.ptr - buf), "9223372036854775807");
}

}  // namespace
}  // namespace facebook::velox::charconv::test
