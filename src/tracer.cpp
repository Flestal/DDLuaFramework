#include "tracer.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ddlua::tracer {
namespace {

constexpr std::uint32_t kMagic = 0x43524444; // "DDRC"
constexpr std::uint16_t kVersion = 1;
constexpr size_t kJumpSize = 12;
constexpr size_t kMaximumTiles = 32;
constexpr size_t kSnapshotSize = 118;

enum class Event : std::uint16_t {
  session_start = 1,
  move_enter = 10,
  move_exit = 11,
  return_roll = 20,
  hero_actor = 29,
  hero_buff = 30,
};

#pragma pack(push, 1)
struct Record {
  std::uint32_t magic = kMagic;
  std::uint16_t version = kVersion;
  std::uint16_t event = 0;
  std::uint32_t record_size = sizeof(Record);
  std::uint32_t process_id = 0;
  std::uint32_t thread_id = 0;
  std::uint32_t depth = 0;
  std::uint32_t flags = 0;
  std::int64_t qpc = 0;
  std::uint64_t sequence = 0;
  std::uint64_t arguments[8]{};
  float chance = 0.0f;
  float torchlight = 0.0f;
  float alternate_torchlight = 0.0f;
  std::uint32_t area_kind = 0;
  std::uint32_t tile_count = 0;
  std::int32_t selected_tile = -1;
  std::uint32_t reserved = 0;
  std::uint32_t before_content[kMaximumTiles]{};
  std::uint32_t after_content[kMaximumTiles]{};
  std::uint32_t tile_knowledge[kMaximumTiles]{};
  std::uint16_t snapshot_size = 0;
  unsigned char snapshot[kSnapshotSize]{};
};
#pragma pack(pop)

static_assert(sizeof(Record) == 640);

struct Hook {
  unsigned char* target = nullptr;
  std::array<unsigned char, 32> original{};
  size_t patch_size = 0;
  void* trampoline = nullptr;
};

struct Layout {
  size_t raid_instance_pointer_rva = 0;
  size_t raid_torchlight = 0;
  size_t raid_alternate_torchlight = 0;
  size_t hero_begin = 0;
  size_t hero_end = 0;
  size_t actor_buff_begin = 0;
  size_t actor_buff_end = 0;
  size_t buff_entry_size = 0;
  size_t buff_stat_type = 0;
  size_t buff_amount = 0;
  size_t buff_id = 0;
  size_t buff_id_capacity = 0;
  size_t area_tile_begin = 0;
  size_t area_tile_end = 0;
  size_t area_kind = 0;
  size_t area_reversed = 0;
  size_t tile_size = 0;
  size_t tile_knowledge = 0;
  size_t tile_content = 0;
};

using MoveToArea = void(__fastcall*)(void* controller, void* destination,
    void* previous, bool player_initiated);
using ReturnTryPlace = bool(__fastcall*)(void* owner, void* area, float chance,
    std::uint32_t content);

Hook g_move_hook;
Hook g_roll_hook;
MoveToArea g_original_move = nullptr;
ReturnTryPlace g_original_roll = nullptr;
Layout g_layout;
unsigned char* g_module = nullptr;
HANDLE g_file = INVALID_HANDLE_VALUE;
SRWLOCK g_file_lock = SRWLOCK_INIT;
LogCallback g_log = nullptr;
std::atomic<std::uint64_t> g_sequence{0};
thread_local std::uint64_t g_current_move = 0;
thread_local std::uint32_t g_move_depth = 0;
thread_local std::uint64_t g_last_hero_dump = ~std::uint64_t{0};

void report(std::string_view level, std::string_view message) noexcept {
  if (g_log) g_log(level, message);
}

bool readable_range(const void* value, size_t size) noexcept {
  if (!value || !size) return false;
  auto cursor = reinterpret_cast<std::uintptr_t>(value);
  const auto limit = cursor + size;
  if (limit < cursor) return false;
  while (cursor < limit) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info)) ||
        info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
      return false;
    }
    const auto end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    if (end <= cursor) return false;
    cursor = std::min(limit, end);
  }
  return true;
}

void write_record(Record& record) noexcept {
  if (g_file == INVALID_HANDLE_VALUE) return;
  record.process_id = GetCurrentProcessId();
  record.thread_id = GetCurrentThreadId();
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  record.qpc = now.QuadPart;
  AcquireSRWLockExclusive(&g_file_lock);
  DWORD written = 0;
  WriteFile(g_file, &record, sizeof(record), &written, nullptr);
  FlushFileBuffers(g_file);
  ReleaseSRWLockExclusive(&g_file_lock);
}

void copy_snapshot(Record& record, const void* source, size_t size) noexcept {
  const size_t count = std::min(size, sizeof(record.snapshot));
  if (!readable_range(source, count)) return;
  std::memcpy(record.snapshot, source, count);
  record.snapshot_size = static_cast<std::uint16_t>(count);
}

void* raid_instance() noexcept {
  if (!g_module || !g_layout.raid_instance_pointer_rva) return nullptr;
  auto** slot = reinterpret_cast<void**>(g_module + g_layout.raid_instance_pointer_rva);
  return readable_range(slot, sizeof(*slot)) ? *slot : nullptr;
}

void capture_torch(Record& record, void* raid) noexcept {
  if (!raid) return;
  auto* bytes = static_cast<unsigned char*>(raid);
  if (readable_range(bytes + g_layout.raid_torchlight, sizeof(float))) {
    record.torchlight = *reinterpret_cast<const float*>(bytes + g_layout.raid_torchlight);
    record.flags |= 0x10;
  }
  if (readable_range(bytes + g_layout.raid_alternate_torchlight, sizeof(float))) {
    record.alternate_torchlight =
        *reinterpret_cast<const float*>(bytes + g_layout.raid_alternate_torchlight);
    record.flags |= 0x20;
  }
}

bool capture_area(Record& record, void* area, bool after) noexcept {
  if (!area) return false;
  auto* bytes = static_cast<unsigned char*>(area);
  if (!readable_range(bytes, std::max({g_layout.area_tile_end + sizeof(void*),
      g_layout.area_kind + sizeof(std::uint32_t),
      g_layout.area_reversed + sizeof(unsigned char)}))) return false;
  const auto begin = *reinterpret_cast<const std::uintptr_t*>(bytes + g_layout.area_tile_begin);
  const auto end = *reinterpret_cast<const std::uintptr_t*>(bytes + g_layout.area_tile_end);
  record.area_kind = *reinterpret_cast<const std::uint32_t*>(bytes + g_layout.area_kind);
  if (*reinterpret_cast<const unsigned char*>(bytes + g_layout.area_reversed)) {
    record.flags |= 0x40;
  }
  if (!begin || end < begin || !g_layout.tile_size ||
      (end - begin) % g_layout.tile_size != 0) return false;
  const size_t total = (end - begin) / g_layout.tile_size;
  if (total > 256) return false;
  record.tile_count = static_cast<std::uint32_t>(total);
  const size_t count = std::min(total, kMaximumTiles);
  for (size_t index = 0; index < count; ++index) {
    auto* tile = reinterpret_cast<const unsigned char*>(begin + index * g_layout.tile_size);
    if (!readable_range(tile, g_layout.tile_content + sizeof(std::uint32_t))) return false;
    const auto content = *reinterpret_cast<const std::uint32_t*>(tile + g_layout.tile_content);
    if (after) record.after_content[index] = content;
    else record.before_content[index] = content;
    if (!after && readable_range(tile + g_layout.tile_knowledge, sizeof(std::uint32_t))) {
      record.tile_knowledge[index] =
          *reinterpret_cast<const std::uint32_t*>(tile + g_layout.tile_knowledge);
    }
  }
  return true;
}

void dump_hero_buffs(std::uint64_t sequence, void* raid) noexcept {
  if (!raid || g_last_hero_dump == sequence) return;
  g_last_hero_dump = sequence;
  auto* bytes = static_cast<unsigned char*>(raid);
  if (!readable_range(bytes + g_layout.hero_begin, sizeof(void*)) ||
      !readable_range(bytes + g_layout.hero_end, sizeof(void*))) return;
  const auto begin = *reinterpret_cast<const std::uintptr_t*>(bytes + g_layout.hero_begin);
  const auto end = *reinterpret_cast<const std::uintptr_t*>(bytes + g_layout.hero_end);
  if (!begin || end < begin || (end - begin) % sizeof(void*) != 0 ||
      end - begin > sizeof(void*) * 16) return;
  size_t hero_index = 0;
  for (auto slot = begin; slot < end; slot += sizeof(void*), ++hero_index) {
    void* actor = *reinterpret_cast<void* const*>(slot);
    if (!actor) continue;
    auto* actor_bytes = static_cast<unsigned char*>(actor);
    if (!readable_range(actor_bytes + g_layout.actor_buff_end, sizeof(void*))) continue;
    const auto buff_begin = *reinterpret_cast<const std::uintptr_t*>(
        actor_bytes + g_layout.actor_buff_begin);
    const auto buff_end = *reinterpret_cast<const std::uintptr_t*>(
        actor_bytes + g_layout.actor_buff_end);
    size_t buff_count = 0;
    if (buff_begin && buff_end >= buff_begin && g_layout.buff_entry_size &&
        (buff_end - buff_begin) % g_layout.buff_entry_size == 0 &&
        buff_end - buff_begin <= g_layout.buff_entry_size * 1024) {
      buff_count = (buff_end - buff_begin) / g_layout.buff_entry_size;
    }
    Record hero{};
    hero.event = static_cast<std::uint16_t>(Event::hero_actor);
    hero.sequence = sequence;
    hero.arguments[0] = reinterpret_cast<std::uint64_t>(raid);
    hero.arguments[1] = reinterpret_cast<std::uint64_t>(actor);
    hero.arguments[2] = hero_index;
    hero.arguments[3] = buff_count;
    write_record(hero);
    for (size_t index = 0; index < buff_count; ++index) {
      auto* entry = reinterpret_cast<const unsigned char*>(
          buff_begin + index * g_layout.buff_entry_size);
      if (!readable_range(entry, g_layout.buff_entry_size)) break;
      Record buff{};
      buff.event = static_cast<std::uint16_t>(Event::hero_buff);
      buff.sequence = sequence;
      buff.arguments[0] = reinterpret_cast<std::uint64_t>(raid);
      buff.arguments[1] = reinterpret_cast<std::uint64_t>(actor);
      buff.arguments[2] = hero_index;
      buff.arguments[3] = index;
      buff.arguments[4] = *reinterpret_cast<const std::uint32_t*>(
          entry + g_layout.buff_stat_type);
      buff.chance = *reinterpret_cast<const float*>(entry + g_layout.buff_amount);
      const char* id = reinterpret_cast<const char*>(entry + g_layout.buff_id);
      const size_t maximum = std::min(g_layout.buff_id_capacity, sizeof(buff.snapshot));
      if (readable_range(id, maximum)) {
        const size_t length = strnlen_s(id, maximum);
        if (length < maximum) copy_snapshot(buff, id, length + 1);
      }
      write_record(buff);
    }
  }
}

void __fastcall hooked_move(void* controller, void* destination, void* previous,
    bool player_initiated) {
  const std::uint64_t parent = g_current_move;
  const std::uint64_t sequence = g_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
  g_current_move = sequence;
  ++g_move_depth;
  Record enter{};
  enter.event = static_cast<std::uint16_t>(Event::move_enter);
  enter.sequence = sequence;
  enter.depth = g_move_depth;
  enter.arguments[0] = reinterpret_cast<std::uint64_t>(controller);
  enter.arguments[1] = reinterpret_cast<std::uint64_t>(destination);
  enter.arguments[2] = reinterpret_cast<std::uint64_t>(previous);
  enter.arguments[3] = player_initiated;
  enter.arguments[4] = parent;
  capture_torch(enter, raid_instance());
  copy_snapshot(enter, destination, sizeof(enter.snapshot));
  write_record(enter);
  g_original_move(controller, destination, previous, player_initiated);
  Record exit = enter;
  exit.event = static_cast<std::uint16_t>(Event::move_exit);
  exit.snapshot_size = 0;
  std::memset(exit.snapshot, 0, sizeof(exit.snapshot));
  capture_torch(exit, raid_instance());
  copy_snapshot(exit, destination, sizeof(exit.snapshot));
  write_record(exit);
  --g_move_depth;
  g_current_move = parent;
}

bool __fastcall hooked_roll(void* owner, void* area, float chance,
    std::uint32_t content) {
  Record record{};
  record.event = static_cast<std::uint16_t>(Event::return_roll);
  record.sequence = g_current_move;
  record.depth = g_move_depth;
  record.arguments[0] = reinterpret_cast<std::uint64_t>(owner);
  record.arguments[1] = reinterpret_cast<std::uint64_t>(area);
  record.arguments[2] = content;
  record.chance = chance;
  void* raid = raid_instance();
  record.arguments[3] = reinterpret_cast<std::uint64_t>(raid);
  capture_torch(record, raid);
  if (capture_area(record, area, false)) record.flags |= 0x01;
  dump_hero_buffs(record.sequence, raid);
  const bool result = g_original_roll(owner, area, chance, content);
  if (result) record.flags |= 0x02;
  if (capture_area(record, area, true)) record.flags |= 0x04;
  const size_t count = std::min<size_t>(record.tile_count, kMaximumTiles);
  for (size_t index = 0; index < count; ++index) {
    if (record.before_content[index] != record.after_content[index]) {
      record.selected_tile = static_cast<std::int32_t>(index);
      break;
    }
  }
  write_record(record);
  return result;
}

bool read_profile_value(const wchar_t* path, std::wstring_view section,
    const wchar_t* key, std::wstring& output) {
  std::wstring section_copy(section);
  std::array<wchar_t, 2048> buffer{};
  const DWORD length = GetPrivateProfileStringW(section_copy.c_str(), key, L"",
      buffer.data(), static_cast<DWORD>(buffer.size()), path);
  if (!length || length >= buffer.size() - 1) return false;
  output.assign(buffer.data(), length);
  return true;
}

bool load_integer(const wchar_t* path, std::wstring_view section,
    const wchar_t* key, size_t& output) {
  std::wstring text;
  if (!read_profile_value(path, section, key, text)) return false;
  wchar_t* end = nullptr;
  const auto value = std::wcstoull(text.c_str(), &end, 0);
  if (end == text.c_str() || *end != L'\0') return false;
  output = static_cast<size_t>(value);
  return true;
}

bool parse_signature(std::wstring_view text, std::vector<unsigned char>& output) {
  size_t cursor = 0;
  while (cursor < text.size()) {
    while (cursor < text.size() && iswspace(text[cursor])) ++cursor;
    if (cursor == text.size()) break;
    wchar_t* end = nullptr;
    std::wstring remainder(text.substr(cursor));
    const auto value = std::wcstoul(remainder.c_str(), &end, 16);
    if (end == remainder.c_str() || value > 0xFF) return false;
    output.push_back(static_cast<unsigned char>(value));
    cursor += static_cast<size_t>(end - remainder.c_str());
  }
  return !output.empty();
}

bool load_point(const wchar_t* path, std::wstring_view section, const wchar_t* name,
    unsigned char*& address, std::vector<unsigned char>& signature) {
  std::wstring rva_key(name);
  rva_key += L"_rva";
  std::wstring bytes_key(name);
  bytes_key += L"_bytes";
  size_t rva = 0;
  std::wstring bytes;
  if (!load_integer(path, section, rva_key.c_str(), rva) ||
      !read_profile_value(path, section, bytes_key.c_str(), bytes) ||
      !parse_signature(bytes, signature)) return false;
  address = g_module + rva;
  return readable_range(address, signature.size()) &&
      std::memcmp(address, signature.data(), signature.size()) == 0;
}

void absolute_jump(unsigned char* output, const void* destination) {
  output[0] = 0x48;
  output[1] = 0xB8;
  const auto value = reinterpret_cast<std::uintptr_t>(destination);
  std::memcpy(output + 2, &value, sizeof(value));
  output[10] = 0xFF;
  output[11] = 0xE0;
}

bool patch_target(Hook& hook, unsigned char* target, const void* replacement,
    size_t patch_size) {
  if (!target || patch_size < kJumpSize || patch_size > hook.original.size()) return false;
  auto* trampoline = static_cast<unsigned char*>(VirtualAlloc(nullptr,
      patch_size + kJumpSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (!trampoline) return false;
  std::memcpy(hook.original.data(), target, patch_size);
  std::memcpy(trampoline, target, patch_size);
  absolute_jump(trampoline + patch_size, target + patch_size);
  std::array<unsigned char, 32> patch{};
  std::fill(patch.begin(), patch.end(), 0x90);
  absolute_jump(patch.data(), replacement);
  DWORD old = 0;
  if (!VirtualProtect(target, patch_size, PAGE_EXECUTE_READWRITE, &old)) {
    VirtualFree(trampoline, 0, MEM_RELEASE);
    return false;
  }
  std::memcpy(target, patch.data(), patch_size);
  FlushInstructionCache(GetCurrentProcess(), target, patch_size);
  DWORD ignored = 0;
  VirtualProtect(target, patch_size, old, &ignored);
  hook.target = target;
  hook.patch_size = patch_size;
  hook.trampoline = trampoline;
  return true;
}

void restore(Hook& hook) noexcept {
  if (!hook.target || !hook.trampoline) return;
  DWORD old = 0;
  if (VirtualProtect(hook.target, hook.patch_size, PAGE_EXECUTE_READWRITE, &old)) {
    std::memcpy(hook.target, hook.original.data(), hook.patch_size);
    FlushInstructionCache(GetCurrentProcess(), hook.target, hook.patch_size);
    DWORD ignored = 0;
    VirtualProtect(hook.target, hook.patch_size, old, &ignored);
  }
  VirtualFree(hook.trampoline, 0, MEM_RELEASE);
  hook = {};
}

bool load_layout(const wchar_t* path, std::wstring_view section, Layout& value) {
  return
      load_integer(path, section, L"raid_instance_pointer_offset", value.raid_instance_pointer_rva) &&
      load_integer(path, section, L"raid_torchlight_offset", value.raid_torchlight) &&
      load_integer(path, section, L"raid_alternate_torchlight_offset", value.raid_alternate_torchlight) &&
      load_integer(path, section, L"hero_actor_begin_offset", value.hero_begin) &&
      load_integer(path, section, L"hero_actor_end_offset", value.hero_end) &&
      load_integer(path, section, L"actor_buff_begin_offset", value.actor_buff_begin) &&
      load_integer(path, section, L"actor_buff_end_offset", value.actor_buff_end) &&
      load_integer(path, section, L"buff_entry_size", value.buff_entry_size) &&
      load_integer(path, section, L"buff_stat_type_offset", value.buff_stat_type) &&
      load_integer(path, section, L"buff_amount_offset", value.buff_amount) &&
      load_integer(path, section, L"buff_id_offset", value.buff_id) &&
      load_integer(path, section, L"buff_id_capacity", value.buff_id_capacity) &&
      load_integer(path, section, L"area_tile_begin_offset", value.area_tile_begin) &&
      load_integer(path, section, L"area_tile_end_offset", value.area_tile_end) &&
      load_integer(path, section, L"area_kind_offset", value.area_kind) &&
      load_integer(path, section, L"area_reversed_offset", value.area_reversed) &&
      load_integer(path, section, L"area_tile_size", value.tile_size) &&
      load_integer(path, section, L"area_tile_knowledge_offset", value.tile_knowledge) &&
      load_integer(path, section, L"area_tile_content_offset", value.tile_content);
}

} // namespace

bool install(std::wstring_view build_id, const wchar_t* profile_path,
    const wchar_t* log_directory, LogCallback log) noexcept {
  g_log = log;
  g_module = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
  if (!g_module || build_id.empty() || !profile_path || !log_directory) return false;

  unsigned char* move = nullptr;
  unsigned char* roll = nullptr;
  std::vector<unsigned char> move_signature;
  std::vector<unsigned char> roll_signature;
  Layout layout;
  if (!load_point(profile_path, build_id, L"raid_move_to_area", move, move_signature) ||
      !load_point(profile_path, build_id, L"corridor_return_try_place", roll, roll_signature) ||
      !load_layout(profile_path, build_id, layout) || move_signature.size() < 16 ||
      roll_signature.size() < 12) {
    report("ERROR", "Corridor tracer profile missing or signature mismatch; tracer disabled");
    return false;
  }

  wchar_t output[MAX_PATH]{};
  _snwprintf_s(output, _TRUNCATE, L"%s\\corridor_trace_%lu.bin", log_directory,
      GetCurrentProcessId());
  g_file = CreateFileW(output, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
  if (g_file == INVALID_HANDLE_VALUE) {
    report("ERROR", "Could not create corridor trace output");
    return false;
  }

  g_layout = layout;
  Record session{};
  session.event = static_cast<std::uint16_t>(Event::session_start);
  session.arguments[0] = reinterpret_cast<std::uint64_t>(g_module);
  session.arguments[1] = g_layout.raid_instance_pointer_rva;
  session.arguments[2] = g_layout.raid_torchlight;
  session.arguments[3] = g_layout.raid_alternate_torchlight;
  write_record(session);

  if (!patch_target(g_roll_hook, roll, reinterpret_cast<const void*>(&hooked_roll), 12)) {
    CloseHandle(g_file);
    g_file = INVALID_HANDLE_VALUE;
    report("ERROR", "Could not install corridor return-roll tracer hook");
    return false;
  }
  g_original_roll = reinterpret_cast<ReturnTryPlace>(g_roll_hook.trampoline);
  if (!patch_target(g_move_hook, move, reinterpret_cast<const void*>(&hooked_move), 16)) {
    restore(g_roll_hook);
    g_original_roll = nullptr;
    CloseHandle(g_file);
    g_file = INVALID_HANDLE_VALUE;
    report("ERROR", "Could not install move-to-area tracer hook; earlier hook restored");
    return false;
  }
  g_original_move = reinterpret_cast<MoveToArea>(g_move_hook.trampoline);
  report("WARN", "Opt-in corridor return tracer installed; remove corridor_trace.enabled after capture");
  return true;
}

} // namespace ddlua::tracer
