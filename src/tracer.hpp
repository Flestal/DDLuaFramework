#pragma once

#include <string_view>

namespace ddlua::tracer {

using LogCallback = void (*)(std::string_view level, std::string_view message) noexcept;

// Installs the opt-in corridor-return diagnostic hooks. The caller must only
// invoke this for a hash-verified supported build and when the marker file is
// present. No trace hooks are installed during normal framework operation.
bool install(std::wstring_view build_id, const wchar_t* profile_path,
    const wchar_t* log_directory, LogCallback log) noexcept;

} // namespace ddlua::tracer
