#pragma once

#include <windows.h>

namespace ddlua {

// 0 = not started, 1 = running, 2 = ready, 3 = init/script error.
void start_async(HMODULE proxy_module) noexcept;
int initialization_state() noexcept;
void test_render_tick() noexcept;
void test_effect_event() noexcept;
bool test_corridor_event() noexcept;

} // namespace ddlua
