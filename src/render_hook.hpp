#pragma once

namespace ddlua::render_hook {

using TickCallback = void (*)() noexcept;
bool install(TickCallback callback) noexcept;

} // namespace ddlua::render_hook

