#pragma once

#include <string_view>

namespace kv {

inline constexpr std::string_view kVersion = "0.1.0";

[[nodiscard]] std::string_view version() noexcept;

} // namespace kv
