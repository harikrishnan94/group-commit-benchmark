#pragma once

#include <chrono>
#include <string>

namespace std {
namespace detail {
static inline std::string to_string(std::chrono::nanoseconds d) {
  return std::to_string(d.count()) + " ns";
}

static inline std::string to_string(std::chrono::microseconds d) {
  return std::to_string(d.count()) + " us";
}

static inline std::string to_string(std::chrono::milliseconds d) {
  return std::to_string(d.count()) + " ms";
}

static inline std::string to_string(std::chrono::seconds d) {
  return std::to_string(d.count()) + " s";
}
} // namespace detail

std::string to_string(std::chrono::nanoseconds d) {
  if (d.count() > 10000) {
    return detail::to_string(
        std::chrono::duration_cast<std::chrono::microseconds>(d));
  }

  return detail::to_string(d);
}

std::string to_string(std::chrono::microseconds d) {
  if (d.count() > 10000) {
    return detail::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(d));
  }

  return detail::to_string(d);
}

std::string to_string(std::chrono::milliseconds d) {
  if (d.count() > 10000) {
    return detail::to_string(
        std::chrono::duration_cast<std::chrono::seconds>(d));
  }

  return detail::to_string(d);
}

std::string to_string(std::chrono::seconds d) { return detail::to_string(d); }
} // namespace std