#include "core.hpp"
#include "game_bridge.hpp"
#include "render_hook.hpp"
#include "tracer.hpp"

#include <bcrypt.h>
#include <lua.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace ddlua {
namespace {

constexpr std::string_view kFrameworkVersion = "0.6.0";
constexpr ULONGLONG kFilePollMilliseconds = 250;
constexpr ULONGLONG kDiscoveryMilliseconds = 2000;
constexpr size_t kMaxEffectName = 64;
constexpr char kEffectContextMetatable[] = "DDLua.EffectContext";
constexpr char kCorridorContextMetatable[] = "DDLua.CorridorReturnContext";
constexpr char kActorRefMetatable[] = "DDLua.ActorRef";

struct Runtime;

struct ScriptContext {
  Runtime* runtime = nullptr;
  std::string id;
  fs::path path;
  unsigned long long observed_write = 0;
  lua_State* state = nullptr;
  int callbacks_ref = LUA_NOREF;
  int effect_handlers_ref = LUA_NOREF;

  ~ScriptContext() {
    if (state) lua_close(state);
  }
};

struct Runtime {
  fs::path root;
  fs::path game_mods_root;
  std::string build_id;
  bool supported_build = false;
  std::vector<std::unique_ptr<ScriptContext>> scripts;
  ULONGLONG last_file_poll = 0;
  ULONGLONG last_discovery = 0;
  LARGE_INTEGER frequency{};
  LARGE_INTEGER previous_tick{};
};

std::atomic<int> g_state{0};
std::atomic<Runtime*> g_runtime{nullptr};
HMODULE g_proxy_module = nullptr;
wchar_t g_root_path[32768]{};
wchar_t g_log_path[32768]{};

std::string utf8(const std::wstring& value) {
  if (value.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  std::string result(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
  return result;
}

void log_line(std::string_view level, std::string_view message) noexcept {
  SYSTEMTIME now{};
  GetLocalTime(&now);
  char prefix[80]{};
  const int prefix_len = std::snprintf(prefix, sizeof(prefix), "%04u-%02u-%02u %02u:%02u:%02u.%03u [%.*s] ",
      now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
      static_cast<int>(level.size()), level.data());
  std::string line(prefix, static_cast<size_t>(prefix_len));
  line.append(message);
  line.append("\r\n");
  const HANDLE file = CreateFileW(g_log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
      nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
  CloseHandle(file);
}

void script_log(const ScriptContext* script, std::string_view level, std::string_view message) {
  log_line(level, "[" + script->id + "] " + std::string(message));
}

std::string windows_error(DWORD code) {
  char* buffer = nullptr;
  const DWORD count = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
      FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, reinterpret_cast<char*>(&buffer), 0, nullptr);
  std::string result = count && buffer ? std::string(buffer, count) : "Windows error " + std::to_string(code);
  if (buffer) LocalFree(buffer);
  while (!result.empty() && (result.back() == '\r' || result.back() == '\n')) result.pop_back();
  return result;
}

bool read_file(const fs::path& path, std::vector<char>& output, bool report_error = true) {
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    if (report_error) log_line("ERROR", "Cannot open " + utf8(path.wstring()) + ": " + windows_error(GetLastError()));
    return false;
  }
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 16 * 1024 * 1024) {
    if (report_error) log_line("ERROR", "Invalid or oversized file: " + utf8(path.wstring()));
    CloseHandle(file);
    return false;
  }
  output.resize(static_cast<size_t>(size.QuadPart));
  DWORD read = 0;
  const bool ok = output.empty() || ReadFile(file, output.data(), static_cast<DWORD>(output.size()), &read, nullptr);
  CloseHandle(file);
  if (!ok || read != output.size()) {
    if (report_error) log_line("ERROR", "Cannot read complete file: " + utf8(path.wstring()));
    return false;
  }
  return true;
}

unsigned long long file_write_time(const fs::path& path) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
  ULARGE_INTEGER value{};
  value.LowPart = data.ftLastWriteTime.dwLowDateTime;
  value.HighPart = data.ftLastWriteTime.dwHighDateTime;
  return value.QuadPart;
}

std::string sha256_file(const fs::path& path) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  HANDLE file = INVALID_HANDLE_VALUE;
  std::vector<unsigned char> object;
  std::array<unsigned char, 32> digest{};
  std::array<unsigned char, 64 * 1024> buffer{};
  std::string result;
  DWORD object_size = 0;
  DWORD ignored = 0;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) goto cleanup;
  if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size),
      sizeof(object_size), &ignored, 0) < 0) goto cleanup;
  object.resize(object_size);
  if (BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) < 0) goto cleanup;
  file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (file == INVALID_HANDLE_VALUE) goto cleanup;
  for (;;) {
    DWORD read = 0;
    if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) goto cleanup;
    if (read == 0) break;
    if (BCryptHashData(hash, buffer.data(), read, 0) < 0) goto cleanup;
  }
  if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) goto cleanup;
  {
    static constexpr char hex[] = "0123456789ABCDEF";
    result.reserve(64);
    for (unsigned char byte : digest) {
      result.push_back(hex[byte >> 4]);
      result.push_back(hex[byte & 0x0f]);
    }
  }
cleanup:
  if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
  if (hash) BCryptDestroyHash(hash);
  if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
  return result;
}

std::string trim(std::string value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
  return value;
}

std::string supported_build_id(std::string_view hash) {
  std::vector<char> data;
  if (!read_file(fs::path(g_root_path) / L"config" / L"supported_builds.ini", data)) return {};
  std::string text(data.begin(), data.end());
  size_t start = 0;
  while (start <= text.size()) {
    const size_t end = text.find_first_of("\r\n", start);
    std::string line = trim(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
    if (!line.empty() && line.front() != '#' && line.front() != ';') {
      const size_t equals = line.find('=');
      if (equals != std::string::npos && trim(line.substr(0, equals)) == hash) return trim(line.substr(equals + 1));
    }
    if (end == std::string::npos) break;
    start = end + 1;
    if (start < text.size() && text[start - 1] == '\r' && text[start] == '\n') ++start;
  }
  return {};
}

ScriptContext* context_from_upvalue(lua_State* state) {
  return static_cast<ScriptContext*>(lua_touserdata(state, lua_upvalueindex(1)));
}

int lua_log(lua_State* state) {
  ScriptContext* script = context_from_upvalue(state);
  std::string message;
  for (int i = 1; i <= lua_gettop(state); ++i) {
    size_t length = 0;
    const char* value = luaL_tolstring(state, i, &length);
    if (i > 1) message.push_back('\t');
    if (value) message.append(value, length);
    lua_pop(state, 1);
  }
  script_log(script, "LUA", message);
  return 0;
}

int lua_is_supported_build(lua_State* state) {
  lua_pushboolean(state, context_from_upvalue(state)->runtime->supported_build);
  return 1;
}

int lua_on(lua_State* state) {
  ScriptContext* script = context_from_upvalue(state);
  const char* event = luaL_checkstring(state, 1);
  luaL_checktype(state, 2, LUA_TFUNCTION);
  if (std::strcmp(event, "render_tick") != 0 &&
      std::strcmp(event, "corridor_return_roll") != 0) {
    return luaL_error(state, "unsupported event '%s'", event);
  }
  lua_rawgeti(state, LUA_REGISTRYINDEX, script->callbacks_ref);
  lua_getfield(state, -1, event);
  if (lua_isnil(state, -1)) {
    lua_pop(state, 1);
    lua_newtable(state);
    lua_pushvalue(state, -1);
    lua_setfield(state, -3, event);
  }
  const lua_Integer next = static_cast<lua_Integer>(lua_rawlen(state, -1) + 1);
  lua_pushvalue(state, 2);
  lua_rawseti(state, -2, next);
  lua_pop(state, 2);
  return 0;
}

struct LuaEffectContext {
  void* battle = nullptr;
  void* performer = nullptr;
  void* target = nullptr;
  bool active = false;
};

struct LuaCorridorContext {
  void* raid = nullptr;
  std::string_view content;
  double native_chance = 0.0;
  double torchlight = 0.0;
  bool active = false;
};

struct LuaActorRef {
  void* battle = nullptr;
  void* actor = nullptr;
  bool* context_active = nullptr;
};

LuaActorRef* checked_actor_ref(lua_State* state, int index = 1) {
  auto* actor = static_cast<LuaActorRef*>(luaL_checkudata(state, index,
      kActorRefMetatable));
  if (!actor->context_active || !*actor->context_active || !actor->actor) {
    luaL_error(state, "actor reference is no longer active");
    return nullptr;
  }
  return actor;
}

void push_actor_ref(lua_State* state, int parent_index, void* battle, void* actor,
    bool* context_active) {
  if (!actor) {
    lua_pushnil(state);
    return;
  }
  parent_index = lua_absindex(state, parent_index);
  auto* reference = static_cast<LuaActorRef*>(lua_newuserdatauv(
      state, sizeof(LuaActorRef), 1));
  *reference = {battle, actor, context_active};
  luaL_getmetatable(state, kActorRefMetatable);
  lua_setmetatable(state, -2);
  lua_pushvalue(state, parent_index);
  lua_setiuservalue(state, -2, 1);
}

int push_actor_array(lua_State* state, int parent_index, void* owner,
    void* battle, std::string_view group, bool* context_active) {
  std::vector<void*> actors;
  if (!game_bridge::collect_actors(owner, group, actors)) {
    return luaL_error(state, "could not enumerate '%.*s' actors",
        static_cast<int>(group.size()), group.data());
  }
  parent_index = lua_absindex(state, parent_index);
  lua_createtable(state, static_cast<int>(actors.size()), 0);
  for (size_t index = 0; index < actors.size(); ++index) {
    push_actor_ref(state, parent_index, battle, actors[index], context_active);
    lua_rawseti(state, -2, static_cast<lua_Integer>(index + 1));
  }
  return 1;
}

int lua_actor_has_buff(lua_State* state) {
  LuaActorRef* actor = checked_actor_ref(state);
  size_t stat_length = 0;
  const char* stat = luaL_checklstring(state, 2, &stat_length);
  if (lua_isnoneornil(state, 3)) {
    lua_pushboolean(state, game_bridge::actor_has_buff(actor->actor,
        std::string_view(stat, stat_length)));
    return 1;
  }
  const double amount = luaL_checknumber(state, 3);
  const double tolerance = luaL_optnumber(state, 4, 0.0001);
  lua_pushboolean(state, game_bridge::actor_has_buff(actor->actor,
      std::string_view(stat, stat_length), amount, tolerance));
  return 1;
}

int lua_actor_has_buff_id(lua_State* state) {
  LuaActorRef* actor = checked_actor_ref(state);
  size_t id_length = 0;
  const char* id = luaL_checklstring(state, 2, &id_length);
  lua_pushboolean(state, game_bridge::actor_has_buff_id(actor->actor,
      std::string_view(id, id_length)));
  return 1;
}

int lua_actor_trigger_dot(lua_State* state) {
  LuaActorRef* actor = checked_actor_ref(state);
  if (!actor->battle) return luaL_error(state, "DoT operations require an effect context");
  size_t dot_length = 0;
  const char* dot = luaL_checklstring(state, 2, &dot_length);
  lua_pushboolean(state, game_bridge::trigger_dot(
      std::string_view(dot, dot_length), actor->battle, actor->actor));
  return 1;
}

int lua_actor_defer_dot(lua_State* state) {
  LuaActorRef* actor = checked_actor_ref(state);
  if (!actor->battle) return luaL_error(state, "DoT operations require an effect context");
  size_t dot_length = 0;
  const char* dot = luaL_checklstring(state, 2, &dot_length);
  lua_pushboolean(state, game_bridge::defer_dot(
      std::string_view(dot, dot_length), actor->battle, actor->actor));
  return 1;
}

int lua_actor_equal(lua_State* state) {
  LuaActorRef* left = checked_actor_ref(state, 1);
  LuaActorRef* right = checked_actor_ref(state, 2);
  lua_pushboolean(state, left->actor == right->actor);
  return 1;
}

LuaCorridorContext* checked_corridor_context(lua_State* state) {
  auto* context = static_cast<LuaCorridorContext*>(luaL_checkudata(state, 1,
      kCorridorContextMetatable));
  if (!context->active) {
    luaL_error(state, "corridor-return context is no longer active");
    return nullptr;
  }
  return context;
}

int lua_corridor_actors(lua_State* state) {
  LuaCorridorContext* context = checked_corridor_context(state);
  size_t group_length = 0;
  const char* group = luaL_checklstring(state, 2, &group_length);
  const std::string_view group_name(group, group_length);
  if (group_name != "heroes") {
    return luaL_error(state, "corridor context can enumerate only 'heroes'");
  }
  return push_actor_array(state, 1, context->raid, nullptr, group_name,
      &context->active);
}

int lua_corridor_index(lua_State* state) {
  LuaCorridorContext* context = checked_corridor_context(state);
  const char* key = luaL_checkstring(state, 2);
  if (std::strcmp(key, "content") == 0) {
    lua_pushlstring(state, context->content.data(), context->content.size());
  } else if (std::strcmp(key, "native_chance") == 0) {
    lua_pushnumber(state, context->native_chance);
  } else if (std::strcmp(key, "torchlight") == 0) {
    lua_pushnumber(state, context->torchlight);
  } else if (std::strcmp(key, "actors") == 0) {
    lua_pushcfunction(state, lua_corridor_actors);
  } else {
    lua_pushnil(state);
  }
  return 1;
}

LuaEffectContext* checked_effect_context(lua_State* state) {
  auto* context = static_cast<LuaEffectContext*>(luaL_checkudata(state, 1,
      kEffectContextMetatable));
  if (!context->active) {
    luaL_error(state, "effect context is no longer active");
    return nullptr;
  }
  return context;
}

int lua_effect_actors(lua_State* state) {
  LuaEffectContext* context = checked_effect_context(state);
  size_t group_length = 0;
  const char* group = luaL_checklstring(state, 2, &group_length);
  const std::string_view group_name(group, group_length);
  if (group_name != "heroes" && group_name != "enemies") {
    return luaL_error(state, "effect context group must be 'heroes' or 'enemies'");
  }
  return push_actor_array(state, 1, context->battle, context->battle,
      group_name, &context->active);
}

int lua_effect_context_index(lua_State* state) {
  LuaEffectContext* context = checked_effect_context(state);
  const char* key = luaL_checkstring(state, 2);
  if (std::strcmp(key, "target") == 0) {
    push_actor_ref(state, 1, context->battle, context->target, &context->active);
  } else if (std::strcmp(key, "performer") == 0) {
    push_actor_ref(state, 1, context->battle, context->performer, &context->active);
  } else if (std::strcmp(key, "actors") == 0) {
    lua_pushcfunction(state, lua_effect_actors);
  } else {
    lua_pushnil(state);
  }
  return 1;
}

int lua_effect(lua_State* state) {
  ScriptContext* script = context_from_upvalue(state);
  size_t name_length = 0;
  const char* name = luaL_checklstring(state, 1, &name_length);
  if (!name_length || name_length > kMaxEffectName ||
      std::memchr(name, '\0', name_length)) {
    return luaL_error(state, "effect name must contain 1..%zu non-NUL bytes", kMaxEffectName);
  }
  luaL_checktype(state, 2, LUA_TTABLE);
  lua_getfield(state, 2, "before_apply");
  const bool has_before = lua_isfunction(state, -1);
  if (!has_before && !lua_isnil(state, -1)) {
    return luaL_error(state, "before_apply must be a function or nil");
  }
  lua_pop(state, 1);
  lua_getfield(state, 2, "after_apply");
  const bool has_after = lua_isfunction(state, -1);
  if (!has_after && !lua_isnil(state, -1)) {
    return luaL_error(state, "after_apply must be a function or nil");
  }
  lua_pop(state, 1);
  if (!has_before && !has_after) {
    return luaL_error(state, "effect handler requires before_apply or after_apply");
  }
  lua_rawgeti(state, LUA_REGISTRYINDEX, script->effect_handlers_ref);
  lua_pushvalue(state, 2);
  lua_setfield(state, -2, std::string(name, name_length).c_str());
  lua_pop(state, 1);
  return 0;
}

int traceback(lua_State* state) {
  const char* message = lua_tostring(state, 1);
  if (message) luaL_traceback(state, state, message, 1);
  else lua_pushliteral(state, "(non-string Lua error)");
  return 1;
}

void open_safe_libraries(lua_State* state) {
  const luaL_Reg libraries[] = {
      {LUA_GNAME, luaopen_base}, {LUA_COLIBNAME, luaopen_coroutine}, {LUA_TABLIBNAME, luaopen_table},
      {LUA_STRLIBNAME, luaopen_string}, {LUA_MATHLIBNAME, luaopen_math}, {LUA_UTF8LIBNAME, luaopen_utf8},
      {nullptr, nullptr}};
  for (const luaL_Reg* library = libraries; library->func; ++library) {
    luaL_requiref(state, library->name, library->func, 1);
    lua_pop(state, 1);
  }
}

void create_effect_context_metatable(lua_State* state) {
  luaL_newmetatable(state, kEffectContextMetatable);
  lua_pushcfunction(state, lua_effect_context_index);
  lua_setfield(state, -2, "__index");
  lua_pushliteral(state, "protected");
  lua_setfield(state, -2, "__metatable");
  lua_pop(state, 1);
}

void create_actor_ref_metatable(lua_State* state) {
  luaL_newmetatable(state, kActorRefMetatable);
  lua_newtable(state);
  lua_pushcfunction(state, lua_actor_has_buff);
  lua_setfield(state, -2, "has_buff");
  lua_pushcfunction(state, lua_actor_has_buff_id);
  lua_setfield(state, -2, "has_buff_id");
  lua_pushcfunction(state, lua_actor_trigger_dot);
  lua_setfield(state, -2, "trigger_dot");
  lua_pushcfunction(state, lua_actor_defer_dot);
  lua_setfield(state, -2, "defer_dot");
  lua_setfield(state, -2, "__index");
  lua_pushcfunction(state, lua_actor_equal);
  lua_setfield(state, -2, "__eq");
  lua_pushliteral(state, "protected");
  lua_setfield(state, -2, "__metatable");
  lua_pop(state, 1);
}

void create_corridor_context_metatable(lua_State* state) {
  luaL_newmetatable(state, kCorridorContextMetatable);
  lua_pushcfunction(state, lua_corridor_index);
  lua_setfield(state, -2, "__index");
  lua_pushliteral(state, "protected");
  lua_setfield(state, -2, "__metatable");
  lua_pop(state, 1);
}

std::unique_ptr<ScriptContext> load_script(Runtime* runtime, const std::string& id, const fs::path& path) {
  auto script = std::make_unique<ScriptContext>();
  script->runtime = runtime;
  script->id = id;
  script->path = path;
  script->observed_write = file_write_time(path);
  std::vector<char> source;
  if (!script->observed_write || !read_file(path, source, false)) {
    script_log(script.get(), "ERROR", "Cannot read init.lua");
    return script;
  }
  script->state = luaL_newstate();
  if (!script->state) {
    script_log(script.get(), "ERROR", "luaL_newstate failed");
    return script;
  }
  lua_State* state = script->state;
  open_safe_libraries(state);
  create_actor_ref_metatable(state);
  create_effect_context_metatable(state);
  create_corridor_context_metatable(state);
  lua_newtable(state);
  script->callbacks_ref = luaL_ref(state, LUA_REGISTRYINDEX);
  lua_newtable(state);
  script->effect_handlers_ref = luaL_ref(state, LUA_REGISTRYINDEX);

  lua_newtable(state);
  lua_pushlightuserdata(state, script.get());
  lua_pushcclosure(state, lua_log, 1);
  lua_setfield(state, -2, "log");
  lua_pushlightuserdata(state, script.get());
  lua_pushcclosure(state, lua_on, 1);
  lua_setfield(state, -2, "on");
  lua_pushlightuserdata(state, script.get());
  lua_pushcclosure(state, lua_is_supported_build, 1);
  lua_setfield(state, -2, "is_supported_build");
  lua_pushlightuserdata(state, script.get());
  lua_pushcclosure(state, lua_effect, 1);
  lua_setfield(state, -2, "effect");
  lua_pushlstring(state, kFrameworkVersion.data(), kFrameworkVersion.size());
  lua_setfield(state, -2, "framework_version");
  lua_pushlstring(state, runtime->build_id.data(), runtime->build_id.size());
  lua_setfield(state, -2, "game_build");
  lua_setglobal(state, "dd");
  lua_pushlightuserdata(state, script.get());
  lua_pushcclosure(state, lua_log, 1);
  lua_setglobal(state, "print");

  lua_pushcfunction(state, traceback);
  const int error_handler = lua_gettop(state);
  const std::string chunk_name = "@" + id + "/init.lua";
  int status = luaL_loadbufferx(state, source.data(), source.size(), chunk_name.c_str(), "t");
  if (status == LUA_OK) status = lua_pcall(state, 0, 0, error_handler);
  if (status != LUA_OK) {
    const char* error = lua_tostring(state, -1);
    script_log(script.get(), "ERROR", std::string("Lua load failed: ") + (error ? error : "unknown error"));
    lua_close(script->state);
    script->state = nullptr;
    script->callbacks_ref = LUA_NOREF;
    return script;
  }
  lua_settop(state, 0);
  script_log(script.get(), "INFO", "Lua script loaded");
  return script;
}

std::vector<std::pair<std::string, fs::path>> discover_scripts(Runtime* runtime) {
  std::vector<std::pair<std::string, fs::path>> found;
  found.emplace_back("core", runtime->root / L"init.lua");
  std::error_code error;
  for (const auto& entry : fs::directory_iterator(runtime->game_mods_root,
      fs::directory_options::skip_permission_denied, error)) {
    if (!entry.is_directory(error)) continue;
    const fs::path lua_directory = entry.path() / L"lua";
    if (!fs::is_directory(lua_directory, error)) continue;
    const fs::path init = lua_directory / L"init.lua";
    if (file_write_time(init)) found.emplace_back(utf8(entry.path().filename().wstring()), init);
  }
  if (found.size() > 1) {
    std::sort(found.begin() + 1, found.end(), [](const auto& left, const auto& right) { return left.first < right.first; });
  }
  return found;
}

void reload_if_changed(Runtime* runtime, std::unique_ptr<ScriptContext>& current) {
  const unsigned long long write = file_write_time(current->path);
  if (!write || write == current->observed_write) return;
  auto candidate = load_script(runtime, current->id, current->path);
  if (candidate->state) {
    script_log(candidate.get(), "INFO", "Hot reload committed");
    current = std::move(candidate);
  } else {
    current->observed_write = candidate->observed_write;
    script_log(current.get(), "WARN", "Hot reload rejected; previous working state retained");
  }
}

void refresh_discovery(Runtime* runtime, bool initial) {
  const auto found = discover_scripts(runtime);
  for (const auto& [id, path] : found) {
    const auto existing = std::find_if(runtime->scripts.begin(), runtime->scripts.end(),
        [&](const auto& script) { return script->id == id; });
    if (existing != runtime->scripts.end()) continue;
    auto loaded = load_script(runtime, id, path);
    if (!initial) script_log(loaded.get(), loaded->state ? "INFO" : "WARN",
        loaded->state ? "Hot-loaded new mod" : "New mod failed to load; waiting for another edit");
    runtime->scripts.push_back(std::move(loaded));
  }
  for (auto it = runtime->scripts.begin(); it != runtime->scripts.end();) {
    if ((*it)->id == "core") { ++it; continue; }
    const bool present = std::any_of(found.begin(), found.end(), [&](const auto& item) { return item.first == (*it)->id; });
    if (present) { ++it; continue; }
    script_log(it->get(), "INFO", "Mod directory removed; state unloaded");
    it = runtime->scripts.erase(it);
  }
}

void dispatch_render_tick(Runtime* runtime, double dt) noexcept {
  for (const auto& script : runtime->scripts) {
    if (!script->state) continue;
    lua_State* state = script->state;
    lua_pushcfunction(state, traceback);
    const int error_handler = lua_gettop(state);
    lua_rawgeti(state, LUA_REGISTRYINDEX, script->callbacks_ref);
    lua_getfield(state, -1, "render_tick");
    if (lua_istable(state, -1)) {
      const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(state, -1));
      for (lua_Integer index = 1; index <= count; ++index) {
        lua_rawgeti(state, -1, index);
        if (!lua_isfunction(state, -1)) {
          lua_pop(state, 1);
          continue;
        }
        lua_pushnumber(state, dt);
        if (lua_pcall(state, 1, 0, error_handler) != LUA_OK) {
          const char* error = lua_tostring(state, -1);
          script_log(script.get(), "ERROR", std::string("render_tick callback failed: ") +
              (error ? error : "unknown error"));
          lua_pop(state, 1);
          lua_pushboolean(state, 0);
          lua_rawseti(state, -2, index);
          script_log(script.get(), "WARN", "Failing callback disabled until the next successful reload");
        }
      }
    }
    lua_settop(state, error_handler - 1);
  }
}

game_bridge::CorridorModifiers dispatch_corridor_return_roll(
    std::string_view content, double native_chance, double torchlight,
    void* raid) noexcept {
  game_bridge::CorridorModifiers total;
  Runtime* runtime = g_runtime.load(std::memory_order_acquire);
  if (!runtime) return total;
  for (const auto& script : runtime->scripts) {
    if (!script->state) continue;
    lua_State* state = script->state;
    const int base = lua_gettop(state);
    lua_pushcfunction(state, traceback);
    const int error_handler = lua_gettop(state);
    lua_rawgeti(state, LUA_REGISTRYINDEX, script->callbacks_ref);
    lua_getfield(state, -1, "corridor_return_roll");
    if (lua_istable(state, -1)) {
      const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(state, -1));
      for (lua_Integer index = 1; index <= count; ++index) {
        lua_rawgeti(state, -1, index);
        if (!lua_isfunction(state, -1)) {
          lua_pop(state, 1);
          continue;
        }
        auto* context = static_cast<LuaCorridorContext*>(lua_newuserdatauv(
            state, sizeof(LuaCorridorContext), 0));
        *context = {raid, content, native_chance, torchlight, true};
        luaL_getmetatable(state, kCorridorContextMetatable);
        lua_setmetatable(state, -2);
        const int status = lua_pcall(state, 1, 1, error_handler);
        context->active = false;
        if (status != LUA_OK) {
          const char* error = lua_tostring(state, -1);
          script_log(script.get(), "ERROR",
              std::string("corridor_return_roll callback failed: ") +
              (error ? error : "unknown error"));
          lua_pop(state, 1);
          lua_pushboolean(state, 0);
          lua_rawseti(state, -2, index);
          script_log(script.get(), "WARN",
              "Failing corridor-return callback disabled until the next successful reload");
          continue;
        }
        if (lua_istable(state, -1)) {
          lua_getfield(state, -1, "chance_multiplier_delta");
          if (!lua_isnil(state, -1)) {
            const double delta = lua_isnumber(state, -1)
                ? lua_tonumber(state, -1) : 0.0;
            if (lua_isnumber(state, -1) && std::isfinite(delta)) {
              total.chance_multiplier_delta = std::clamp(
                  total.chance_multiplier_delta + delta, -1.0, 7.0);
            } else {
              script_log(script.get(), "WARN",
                  "Invalid chance_multiplier_delta ignored");
            }
          }
          lua_pop(state, 1);
          lua_getfield(state, -1, "extra_rolls");
          if (!lua_isnil(state, -1)) {
            const lua_Integer extra = lua_isinteger(state, -1)
                ? lua_tointeger(state, -1) : -1;
            if (extra >= 0) {
              total.extra_rolls = std::min<unsigned int>(4,
                  total.extra_rolls + static_cast<unsigned int>(
                      std::min<lua_Integer>(extra, 4)));
            } else {
              script_log(script.get(), "WARN", "Invalid extra_rolls ignored");
            }
          }
          lua_pop(state, 1);
        } else if (!lua_isnil(state, -1)) {
          script_log(script.get(), "WARN",
              "corridor_return_roll must return a table or nil; result ignored");
        }
        lua_pop(state, 1);
      }
    }
    lua_settop(state, base);
  }
  return total;
}

bool script_observes_effect(ScriptContext* script, std::string_view effect_name) noexcept {
  if (!script || !script->state || script->effect_handlers_ref == LUA_NOREF) return false;
  lua_State* state = script->state;
  const int base = lua_gettop(state);
  lua_rawgeti(state, LUA_REGISTRYINDEX, script->effect_handlers_ref);
  lua_pushlstring(state, effect_name.data(), effect_name.size());
  lua_rawget(state, -2);
  const bool observed = lua_istable(state, -1);
  lua_settop(state, base);
  return observed;
}

bool is_effect_observed(std::string_view effect_name) noexcept {
  Runtime* runtime = g_runtime.load(std::memory_order_acquire);
  if (!runtime) return false;
  for (const auto& script : runtime->scripts) {
    if (script_observes_effect(script.get(), effect_name)) return true;
  }
  return false;
}

bool invoke_effect_handler(ScriptContext* script, std::string_view effect_name,
    const char* phase, void* battle, void* performer, void* target,
    bool default_result) noexcept {
  if (!script_observes_effect(script, effect_name)) return default_result;
  lua_State* state = script->state;
  const int base = lua_gettop(state);
  lua_pushcfunction(state, traceback);
  const int error_handler = lua_gettop(state);
  lua_rawgeti(state, LUA_REGISTRYINDEX, script->effect_handlers_ref);
  lua_pushlstring(state, effect_name.data(), effect_name.size());
  lua_rawget(state, -2);
  lua_getfield(state, -1, phase);
  if (!lua_isfunction(state, -1)) {
    lua_settop(state, base);
    return default_result;
  }

  auto* context = static_cast<LuaEffectContext*>(lua_newuserdatauv(state,
      sizeof(LuaEffectContext), 0));
  *context = {battle, performer, target, true};
  luaL_getmetatable(state, kEffectContextMetatable);
  lua_setmetatable(state, -2);
  const int results = std::strcmp(phase, "before_apply") == 0 ? 1 : 0;
  const int status = lua_pcall(state, 1, results, error_handler);
  context->active = false;
  if (status != LUA_OK) {
    const char* error = lua_tostring(state, -1);
    script_log(script, "ERROR", std::string("effect ") + phase + " callback failed: " +
        (error ? error : "unknown error"));
    lua_settop(state, base);
    return default_result;
  }
  bool result = default_result;
  if (results == 1 && lua_isboolean(state, -1)) result = lua_toboolean(state, -1) != 0;
  lua_settop(state, base);
  return result;
}

bool before_effect_target(std::string_view effect_name, void* battle,
    void* performer, void* target) noexcept {
  Runtime* runtime = g_runtime.load(std::memory_order_acquire);
  if (!runtime) return true;
  bool allow = true;
  for (const auto& script : runtime->scripts) {
    if (!invoke_effect_handler(script.get(), effect_name, "before_apply", battle,
        performer, target, true)) allow = false;
  }
  return allow;
}

void after_effect_target(std::string_view effect_name, void* battle,
    void* performer, void* target) noexcept {
  Runtime* runtime = g_runtime.load(std::memory_order_acquire);
  if (!runtime) return;
  for (const auto& script : runtime->scripts) {
    invoke_effect_handler(script.get(), effect_name, "after_apply", battle,
        performer, target, true);
  }
}

void on_render_tick() noexcept {
  Runtime* runtime = g_runtime.load(std::memory_order_acquire);
  if (!runtime) return;
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  double dt = runtime->frequency.QuadPart
      ? static_cast<double>(now.QuadPart - runtime->previous_tick.QuadPart) / runtime->frequency.QuadPart : 0.0;
  runtime->previous_tick = now;
  dt = std::clamp(dt, 0.0, 0.25);

  const ULONGLONG milliseconds = GetTickCount64();
  if (milliseconds - runtime->last_file_poll >= kFilePollMilliseconds) {
    runtime->last_file_poll = milliseconds;
    for (auto& script : runtime->scripts) reload_if_changed(runtime, script);
  }
  if (milliseconds - runtime->last_discovery >= kDiscoveryMilliseconds) {
    runtime->last_discovery = milliseconds;
    refresh_discovery(runtime, false);
  }
  dispatch_render_tick(runtime, dt);
}

DWORD WINAPI initialize_thread(void*) noexcept {
  wchar_t module_path[32768]{};
  const DWORD module_length = GetModuleFileNameW(g_proxy_module, module_path, static_cast<DWORD>(std::size(module_path)));
  if (!module_length || module_length == std::size(module_path)) {
    g_state.store(3, std::memory_order_release);
    return 0;
  }
  const fs::path root = fs::path(module_path).parent_path() / L"ddlua";
  const fs::path log_directory = root / L"logs";
  const fs::path log_path = log_directory / L"ddlua.log";
  wcsncpy_s(g_root_path, root.c_str(), _TRUNCATE);
  wcsncpy_s(g_log_path, log_path.c_str(), _TRUNCATE);
  CreateDirectoryW(root.c_str(), nullptr);
  CreateDirectoryW(log_directory.c_str(), nullptr);
  log_line("INFO", "DDLua Framework 0.6.0 bootstrap started");

  wchar_t process_path[32768]{};
  const DWORD process_length = GetModuleFileNameW(nullptr, process_path, static_cast<DWORD>(std::size(process_path)));
  const std::string hash = process_length ? sha256_file(process_path) : std::string{};
  std::string build_id;
  if (hash.empty()) log_line("WARN", "Process SHA-256 could not be calculated; game bridges disabled");
  else {
    log_line("INFO", "Process SHA-256: " + hash);
    build_id = supported_build_id(hash);
    if (build_id.empty()) log_line("WARN", "Unsupported executable; all game hooks and bridges are disabled (fail closed)");
    else log_line("INFO", "Supported Darkest Dungeon build profile: " + build_id);
  }

  auto* runtime = new Runtime();
  runtime->root = root;
  const fs::path dll_directory = fs::path(module_path).parent_path();
  fs::path game_root = dll_directory;
  if (_wcsicmp(dll_directory.filename().c_str(), L"win64") == 0 &&
      _wcsicmp(dll_directory.parent_path().filename().c_str(), L"_windows") == 0) {
    game_root = dll_directory.parent_path().parent_path();
  }
  runtime->game_mods_root = game_root / L"mods";
  runtime->build_id = build_id;
  runtime->supported_build = !build_id.empty();
  log_line("INFO", "Lua mod discovery root: " + utf8(runtime->game_mods_root.wstring()));
  QueryPerformanceFrequency(&runtime->frequency);
  QueryPerformanceCounter(&runtime->previous_tick);
  refresh_discovery(runtime, true);
  const bool core_loaded = !runtime->scripts.empty() && runtime->scripts.front()->state;
  g_runtime.store(runtime, std::memory_order_release);

  bool hook_ready = true;
  if (runtime->supported_build) {
    hook_ready = render_hook::install(on_render_tick);
    log_line(hook_ready ? "INFO" : "ERROR", hook_ready
        ? "SDL_GL_SwapWindow render boundary installed"
        : "SDL_GL_SwapWindow import hook failed; render_tick and hot reload disabled");
    const fs::path bridge_profile = root / L"config" / L"game_bridge_profiles.ini";
    const bool buff_id_probe_enabled = fs::exists(root / L"config" /
        L"buff_id_probe.enabled");
    const bool corridor_trace_enabled = fs::exists(root / L"config" /
        L"corridor_trace.enabled");
    const std::wstring build_id_wide(runtime->build_id.begin(), runtime->build_id.end());
    const game_bridge::HostCallbacks callbacks{
        is_effect_observed, before_effect_target, after_effect_target,
        dispatch_corridor_return_roll};
    const bool bridge_ready = game_bridge::install(build_id_wide, bridge_profile.c_str(),
        buff_id_probe_enabled, !corridor_trace_enabled, callbacks,
        [](std::string_view level, std::string_view message) noexcept {
          log_line(level, message);
        });
    if (!bridge_ready) log_line("WARN", "Internal effect bridge is unavailable; Lua runtime remains active");
    if (corridor_trace_enabled) {
      const bool tracer_ready = tracer::install(build_id_wide, bridge_profile.c_str(),
          log_directory.c_str(),
          [](std::string_view level, std::string_view message) noexcept {
            log_line(level, message);
          });
      if (!tracer_ready) log_line("ERROR", "Opt-in corridor tracer could not be installed");
    }
  }
  g_state.store(core_loaded && hook_ready ? 2 : 3, std::memory_order_release);
  return 0;
}

} // namespace

void start_async(HMODULE proxy_module) noexcept {
  int expected = 0;
  if (!g_state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) return;
  g_proxy_module = proxy_module;
  HANDLE thread = CreateThread(nullptr, 0, initialize_thread, nullptr, 0, nullptr);
  if (!thread) {
    g_state.store(3, std::memory_order_release);
    return;
  }
  CloseHandle(thread);
}

int initialization_state() noexcept { return g_state.load(std::memory_order_acquire); }

void test_render_tick() noexcept { on_render_tick(); }

void test_effect_event() noexcept {
  before_effect_target("ddlua_test_effect", nullptr, nullptr, nullptr);
  after_effect_target("ddlua_test_effect", nullptr, nullptr, nullptr);
}

bool test_corridor_event() noexcept {
  const auto result = dispatch_corridor_return_roll(
      "battle", 0.125, 0.0, nullptr);
  return std::fabs(result.chance_multiplier_delta - 0.5) < 0.000001 &&
      result.extra_rolls == 1;
}

} // namespace ddlua
