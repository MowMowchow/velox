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
 * SHIM: provides std::to_chars-equivalent for __int128 / unsigned __int128
 * when the C++ standard library lacks the overload.
 *
 * libstdc++ (GCC) does NOT ship to_chars(__int128) at any version through
 * libstdc++-15 as of 2026-05. libc++ 12+ provides it as a vendor extension.
 * This shim lets Velox compile cleanly under the gcc-13 + libstdc++-13
 * single-toolchain on Ubuntu 24.04 LTS without forcing downstream consumers
 * to switch to clang+libc++.
 *
 * The shim is a thin wrapper:
 *   - On libc++ 12+: forwards to native std::to_chars.
 *   - On libstdc++ (any version) and other stdlibs: implements an
 *     __int128 / unsigned __int128 fallback whose semantics match the
 *     3-arg std::to_chars contract (decimal base, no leading zeros).
 *   - For all other integral types (int, long, long long, etc.): forwards
 *     to native std::to_chars unconditionally -- guaranteed to exist by
 *     the C++17 standard.
 *
 * Contract guarantees (matching std::to_chars):
 *   - On success: result.ptr points one past the last written digit;
 *     result.ec == std::errc{}.
 *   - On insufficient buffer: result.ptr == last;
 *     result.ec == std::errc::value_too_large; the buffer contents are
 *     unspecified (caller must not consume them).
 *   - Never throws, never allocates.
 *   - Negative inputs: a leading '-' is written, followed by the magnitude.
 *     Handles INT128_MIN correctly via two's-complement magnitude conversion.
 */

#pragma once

#include <charconv>
#include <cstddef>
#include <system_error>
#include <type_traits>

namespace facebook::velox::charconv {

// ---------------------------------------------------------------------------
// __int128 / unsigned __int128 path
// ---------------------------------------------------------------------------

#if defined(_LIBCPP_VERSION) && _LIBCPP_VERSION >= 12000
// libc++ 12+ provides std::to_chars(__int128) as a vendor extension.
inline std::to_chars_result to_chars_int128(
    char* first,
    char* last,
    __int128 value) {
  return std::to_chars(first, last, value);
}

inline std::to_chars_result to_chars_int128(
    char* first,
    char* last,
    unsigned __int128 value) {
  return std::to_chars(first, last, value);
}
#else
// libstdc++ and other stdlibs lack __int128 overloads. Provide a fallback.

inline std::to_chars_result to_chars_int128(
    char* first,
    char* last,
    unsigned __int128 value) {
  // Count digits required (correctly returns 1 for value == 0).
  unsigned __int128 v = value;
  std::size_t len = 0;
  do {
    ++len;
    v /= 10;
  } while (v != 0);

  if (static_cast<std::size_t>(last - first) < len) {
    return {last, std::errc::value_too_large};
  }

  // Write digits in reverse into the slice [first, first+len).
  char* const end = first + len;
  char* out = end;
  v = value;
  for (std::size_t i = 0; i < len; ++i) {
    *--out = static_cast<char>('0' + static_cast<int>(v % 10));
    v /= 10;
  }
  return {end, std::errc{}};
}

inline std::to_chars_result to_chars_int128(
    char* first,
    char* last,
    __int128 value) {
  if (value < 0) {
    if (first == last) {
      return {last, std::errc::value_too_large};
    }
    *first = '-';
    // INT128_MIN edge case: -INT128_MIN overflows in signed __int128.
    // Bit-cast to unsigned and negate via two's-complement: the unsigned
    // expression -static_cast<unsigned __int128>(value) yields the correct
    // magnitude (2^127 for INT128_MIN) by modular wrap-around.
    const unsigned __int128 magnitude =
        -static_cast<unsigned __int128>(value);
    return to_chars_int128(first + 1, last, magnitude);
  }
  return to_chars_int128(first, last, static_cast<unsigned __int128>(value));
}
#endif

// ---------------------------------------------------------------------------
// Generic forwarding overload for all other integral types.
//
// Allows uniform call-site usage from template code (e.g.
// DecimalUtil::castToString<T>) where T may be int32_t, int64_t, int128_t,
// or uint128_t. For non-128-bit T, this delegates to native std::to_chars,
// which the C++17 standard requires to exist for every standard integer type.
// ---------------------------------------------------------------------------

template <
    typename T,
    typename = std::enable_if_t<
        std::is_integral_v<T> &&
        !std::is_same_v<std::remove_cv_t<T>, __int128> &&
        !std::is_same_v<std::remove_cv_t<T>, unsigned __int128>>>
inline std::to_chars_result to_chars_int128(char* first, char* last, T value) {
  return std::to_chars(first, last, value);
}

}  // namespace facebook::velox::charconv
