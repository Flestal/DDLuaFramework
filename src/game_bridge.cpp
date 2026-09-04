#include "game_bridge.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace ddlua::game_bridge {
void __fastcall flush_deferred_dots(void* battle) noexcept;

namespace {

constexpr size_t kPatchSize = 12;
constexpr size_t kMaxEffectName = 64;
constexpr size_t kMaximumDeferredDots = 64;
constexpr unsigned int kMaximumExtraCorridorRolls = 4;

using NamedEffect = void(__fastcall*)(void* battle, const char* effect_name, void* performer,
    void* primary_target, std::uint32_t target_flags, bool use_performer_effects);
using ApplyEffect = void(__fastcall*)(void* battle, void* performer, void* target,
    void* effect_definition, float random_value, bool flag, float scale,
    std::uint32_t effect_flags, std::uint32_t source_rank);
using DotTick = void(__fastcall*)(void* battle, void* actor);
using HasDot = bool(__fastcall*)(void* actor);
using StateTransition = void(__fastcall*)(void* state_machine, std::uint32_t next_state);
using ReturnTryPlace = bool(__fastcall*)(void* owner, void* area, float chance,
    std::uint32_t content);

struct Hook {
  unsigned char* target = nullptr;
  std::array<unsigned char, 32> original{};
  size_t patch_size = 0;
  void* trampoline = nullptr;
};

struct DeferredDot {
  std::string name;
  void* battle = nullptr;
  void* actor = nullptr;
};

struct Layout {
  size_t actor_buff_begin = 0;
  size_t actor_buff_end = 0;
  size_t buff_entry_size = 0;
  size_t buff_stat_type = 0;
  size_t buff_amount = 0;
  size_t buff_id = 0;
  size_t buff_id_capacity = 0;
  size_t state_machine_offset = 0;
  size_t state_value_offset = 0;
  std::uint32_t action_end_state = 0;
  size_t hero_actor_begin = 0;
  size_t hero_actor_end = 0;
  size_t enemy_actor_begin = 0;
  size_t enemy_actor_end = 0;
  size_t raid_instance_pointer = 0;
  size_t raid_torchlight = 0;
  std::unordered_map<std::string, std::uint32_t> stats;
};

Hook g_named_hook;
Hook g_apply_hook;
Hook g_state_transition_hook;
Hook g_corridor_return_hook;
NamedEffect g_original_named = nullptr;
ApplyEffect g_original_apply = nullptr;
StateTransition g_original_state_transition = nullptr;
ReturnTryPlace g_original_corridor_return = nullptr;
std::unordered_map<std::string, DotTick> g_dot_ticks;
std::unordered_map<std::string, HasDot> g_dot_predicates;
Layout g_layout;
HostCallbacks g_callbacks;
LogCallback g_log = nullptr;
bool g_buff_id_probe_enabled = false;
unsigned char* g_module = nullptr;
SRWLOCK g_deferred_lock = SRWLOCK_INIT;
std::vector<DeferredDot> g_deferred_dots;

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
    const auto region_end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) +
        info.RegionSize;
    if (region_end <= cursor) return false;
    cursor = std::min(limit, region_end);
  }
  return true;
}

bool candidate_msvc_string(const unsigned char* object, std::string& output) noexcept {
  if (!readable_range(object, 32)) return false;
  const size_t length = *reinterpret_cast<const size_t*>(object + 0x10);
  const size_t capacity = *reinterpret_cast<const size_t*>(object + 0x18);
  if (!length || length > 127 || capacity < length || capacity > 4096) return false;
  const char* data = capacity < 16
      ? reinterpret_cast<const char*>(object)
      : *reinterpret_cast<const char* const*>(object);
  if (!readable_range(data, length + 1) || data[length] != '\0') return false;
  for (size_t index = 0; index < length; ++index) {
    const unsigned char character = static_cast<unsigned char>(data[index]);
    if (character < 0x20 || character > 0x7E) return false;
  }
  output.assign(data, length);
  return true;
}

void probe_actor_buffs(void* actor, std::string_view stat_name) noexcept {
  if (!g_buff_id_probe_enabled || !actor) return;
  const auto stat = g_layout.stats.find(std::string(stat_name));
  if (stat == g_layout.stats.end() || !g_layout.buff_entry_size) return;
  const auto base = reinterpret_cast<std::uintptr_t>(actor);
  const auto begin = *reinterpret_cast<const std::uintptr_t*>(base + g_layout.actor_buff_begin);
  const auto end = *reinterpret_cast<const std::uintptr_t*>(base + g_layout.actor_buff_end);
  if (!begin || end < begin || (end - begin) % g_layout.buff_entry_size != 0 ||
      end - begin > g_layout.buff_entry_size * 1024) return;
  for (std::uintptr_t entry = begin; entry < end; entry += g_layout.buff_entry_size) {
    if (*reinterpret_cast<const std::uint32_t*>(entry + g_layout.buff_stat_type) !=
        stat->second) continue;
    std::ostringstream line;
    line << "BUFF_ID_PROBE actor=0x" << std::hex << base << " entry=0x" << entry
         << " stat=0x" << stat->second << std::dec << " amount="
         << *reinterpret_cast<const float*>(entry + g_layout.buff_amount)
         << " strings=";
    bool found = false;
    const auto* bytes = reinterpret_cast<const unsigned char*>(entry);
    for (size_t offset = 0; offset + 32 <= g_layout.buff_entry_size; offset += 8) {
      std::string candidate;
      if (!candidate_msvc_string(bytes + offset, candidate)) continue;
      if (found) line << ',';
      line << "0x" << std::hex << offset << std::dec << ':' << candidate;
      found = true;
    }
    if (!found) line << "<none>";
    line << " raw=" << std::hex << std::setfill('0');
    for (size_t offset = 0; offset < g_layout.buff_entry_size; ++offset) {
      line << std::setw(2) << static_cast<unsigned int>(bytes[offset]);
    }
    report("PROBE", line.str());
  }
}

std::string_view bounded_effect_name(const char* value) noexcept {
  if (!value) return {};
  return {value, strnlen_s(value, kMaxEffectName)};
}

void __fastcall hooked_apply(void* battle, void* performer, void* target,
    void* effect_definition, float random_value, bool flag, float scale,
    std::uint32_t effect_flags, std::uint32_t source_rank) {
  const std::string_view effect_name = bounded_effect_name(
      static_cast<const char*>(effect_definition));
  const bool observed = !effect_name.empty() && g_callbacks.is_effect_observed &&
      g_callbacks.is_effect_observed(effect_name);
  if (observed && g_callbacks.before_effect_target &&
      !g_callbacks.before_effect_target(effect_name, battle, performer, target)) {
    return;
  }
  g_original_apply(battle, performer, target, effect_definition, random_value, flag,
      scale, effect_flags, source_rank);
  if (observed && g_callbacks.after_effect_target) {
    g_callbacks.after_effect_target(effect_name, battle, performer, target);
  }
}

void __fastcall hooked_named(void* battle, const char* effect_name, void* performer,
    void* primary_target, std::uint32_t target_flags, bool use_performer_effects) {
  g_original_named(battle, effect_name, performer, primary_target, target_flags,
      use_performer_effects);
}

void __fastcall hooked_state_transition(void* state_machine,
    std::uint32_t next_state) {
  g_original_state_transition(state_machine, next_state);
  if (!state_machine || next_state != g_layout.action_end_state) return;
  const auto current_state = *reinterpret_cast<const std::uint32_t*>(
      reinterpret_cast<const unsigned char*>(state_machine) +
      g_layout.state_value_offset);
  if (current_state != next_state) return;
  auto* battle = reinterpret_cast<unsigned char*>(state_machine) -
      g_layout.state_machine_offset;
  flush_deferred_dots(battle);
}

void* current_raid() noexcept {
  if (!g_module || !g_layout.raid_instance_pointer) return nullptr;
  auto** slot = reinterpret_cast<void**>(g_module + g_layout.raid_instance_pointer);
  return readable_range(slot, sizeof(*slot)) ? *slot : nullptr;
}

std::string_view corridor_content_name(std::uint32_t content) noexcept {
  switch (content) {
    case 0: return "nothing";
    case 1: return "battle";
    case 2: return "ambush";
    case 3: return "trap";
    case 4: return "obstacle";
    case 5: return "happening";
    case 6: return "guarded_curio";
    case 7: return "curio";
    case 8: return "hunger";
    case 9: return "treasure";
    case 10: return "guarded_treasure";
    case 11: return "ambush_curio";
    case 12: return "ambush_treasure";
    case 13: return "hidden_door";
    case 14: return "prisoner";
    default: return {};
  }
}

bool __fastcall hooked_corridor_return(void* owner, void* area, float chance,
    std::uint32_t content) {
  const std::string_view symbolic_content = corridor_content_name(content);
  void* raid = current_raid();
  if (!g_callbacks.corridor_return_roll || symbolic_content.empty() || !raid) {
    return g_original_corridor_return(owner, area, chance, content);
  }
  auto* torch_address = static_cast<unsigned char*>(raid) + g_layout.raid_torchlight;
  if (!readable_range(torch_address, sizeof(float))) {
    return g_original_corridor_return(owner, area, chance, content);
  }
  const float torchlight = *reinterpret_cast<const float*>(torch_address);
  CorridorModifiers modifiers = g_callbacks.corridor_return_roll(
      symbolic_content, chance, torchlight, raid);
  if (!std::isfinite(modifiers.chance_multiplier_delta)) {
    modifiers.chance_multiplier_delta = 0.0;
  }
  const double multiplier = std::clamp(1.0 + modifiers.chance_multiplier_delta,
      0.0, 8.0);
  const float adjusted_chance = static_cast<float>(std::clamp(
      static_cast<double>(chance) * multiplier, 0.0, 1.0));
  const unsigned int extra_rolls = std::min(modifiers.extra_rolls,
      kMaximumExtraCorridorRolls);
  bool placed = false;
  for (unsigned int roll = 0; roll <= extra_rolls; ++roll) {
    placed = g_original_corridor_return(owner, area, adjusted_chance, content) || placed;
  }
  return placed;
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

bool parse_integer(const std::wstring& text, unsigned long long& output) {
  wchar_t* end = nullptr;
  output = std::wcstoull(text.c_str(), &end, 0);
  return end != text.c_str() && *end == L'\0';
}

bool load_integer(const wchar_t* path, std::wstring_view section,
    const wchar_t* key, unsigned long long& output) {
  std::wstring text;
  return read_profile_value(path, section, key, text) && parse_integer(text, output);
}

std::vector<std::pair<std::wstring, std::wstring>> load_section(
    const wchar_t* path, std::wstring_view section) {
  std::wstring section_copy(section);
  std::vector<wchar_t> buffer(32768);
  const DWORD length = GetPrivateProfileSectionW(section_copy.c_str(), buffer.data(),
      static_cast<DWORD>(buffer.size()), path);
  std::vector<std::pair<std::wstring, std::wstring>> values;
  if (!length || length >= buffer.size() - 2) return values;
  for (const wchar_t* item = buffer.data(); *item; item += std::wcslen(item) + 1) {
    const wchar_t* equals = std::wcschr(item, L'=');
    if (!equals) continue;
    values.emplace_back(std::wstring(item, equals), std::wstring(equals + 1));
  }
  return values;
}

bool ascii_identifier(std::wstring_view input, std::string& output) {
  output.clear();
  output.reserve(input.size());
  for (const wchar_t character : input) {
    if (character < 0x21 || character > 0x7E) return false;
    output.push_back(static_cast<char>(character));
  }
  return !output.empty();
}

bool parse_bytes(const std::wstring& text, std::vector<unsigned char>& output) {
  const wchar_t* cursor = text.c_str();
  wchar_t* end = nullptr;
  while (*cursor) {
    while (*cursor == L' ' || *cursor == L'\t') ++cursor;
    if (!*cursor) break;
    const unsigned long value = std::wcstoul(cursor, &end, 16);
    if (end == cursor || value > 0xFF) return false;
    output.push_back(static_cast<unsigned char>(value));
    cursor = end;
  }
  return !output.empty();
}

bool load_point(const wchar_t* path, std::wstring_view section, const wchar_t* name,
    unsigned char* module, unsigned char*& address, std::vector<unsigned char>& signature) {
  std::wstring rva_key(name);
  rva_key += L"_rva";
  std::wstring bytes_key(name);
  bytes_key += L"_bytes";
  std::wstring rva_text;
  std::wstring bytes_text;
  unsigned long long rva = 0;
  if (!read_profile_value(path, section, rva_key.c_str(), rva_text) ||
      !read_profile_value(path, section, bytes_key.c_str(), bytes_text) ||
      !parse_integer(rva_text, rva) || rva > 0x7FFFFFFF ||
      !parse_bytes(bytes_text, signature)) return false;
  address = module + rva;
  return std::memcmp(address, signature.data(), signature.size()) == 0;
}

bool load_layout(const wchar_t* path, std::wstring_view section, Layout& layout) {
  unsigned long long values[16]{};
  const wchar_t* keys[] = {L"actor_buff_begin_offset", L"actor_buff_end_offset",
      L"buff_entry_size", L"buff_stat_type_offset", L"buff_amount_offset",
      L"buff_id_offset", L"buff_id_capacity",
      L"battle_state_machine_offset", L"battle_state_value_offset",
      L"battle_action_end_state", L"hero_actor_begin_offset",
      L"hero_actor_end_offset", L"enemy_actor_begin_offset",
      L"enemy_actor_end_offset", L"raid_instance_pointer_offset",
      L"raid_torchlight_offset"};
  for (size_t index = 0; index < std::size(keys); ++index) {
    if (!load_integer(path, section, keys[index], values[index])) return false;
  }
  layout.actor_buff_begin = static_cast<size_t>(values[0]);
  layout.actor_buff_end = static_cast<size_t>(values[1]);
  layout.buff_entry_size = static_cast<size_t>(values[2]);
  layout.buff_stat_type = static_cast<size_t>(values[3]);
  layout.buff_amount = static_cast<size_t>(values[4]);
  layout.buff_id = static_cast<size_t>(values[5]);
  layout.buff_id_capacity = static_cast<size_t>(values[6]);
  layout.state_machine_offset = static_cast<size_t>(values[7]);
  layout.state_value_offset = static_cast<size_t>(values[8]);
  layout.action_end_state = static_cast<std::uint32_t>(values[9]);
  layout.hero_actor_begin = static_cast<size_t>(values[10]);
  layout.hero_actor_end = static_cast<size_t>(values[11]);
  layout.enemy_actor_begin = static_cast<size_t>(values[12]);
  layout.enemy_actor_end = static_cast<size_t>(values[13]);
  layout.raid_instance_pointer = static_cast<size_t>(values[14]);
  layout.raid_torchlight = static_cast<size_t>(values[15]);
  for (const auto& [key, value] : load_section(path, section)) {
    if (!key.starts_with(L"stat_")) continue;
    unsigned long long parsed = 0;
    if (!parse_integer(value, parsed) || parsed > UINT32_MAX) return false;
    std::string name;
    if (!ascii_identifier(std::wstring_view(key).substr(5), name)) return false;
    layout.stats.emplace(std::move(name), static_cast<std::uint32_t>(parsed));
  }
  return layout.actor_buff_begin && layout.actor_buff_end && layout.buff_entry_size &&
      layout.buff_id_capacity && layout.buff_id < layout.buff_entry_size &&
      layout.buff_id_capacity <= layout.buff_entry_size - layout.buff_id &&
      layout.state_machine_offset && layout.state_value_offset &&
      layout.action_end_state && layout.hero_actor_begin < layout.hero_actor_end &&
      layout.enemy_actor_begin < layout.enemy_actor_end &&
      layout.raid_instance_pointer && layout.raid_torchlight && !layout.stats.empty();
}

bool load_dot_ticks(const wchar_t* path, std::wstring_view section,
    unsigned char* module, std::unordered_map<std::string, DotTick>& output) {
  constexpr std::wstring_view suffix = L"_dot_tick_rva";
  for (const auto& [key, ignored] : load_section(path, section)) {
    if (!key.ends_with(suffix)) continue;
    const std::wstring point_name = key.substr(0, key.size() - 4);
    const std::wstring_view wide_dot_name =
        std::wstring_view(key).substr(0, key.size() - suffix.size());
    std::string dot_name;
    if (!ascii_identifier(wide_dot_name, dot_name)) return false;
    unsigned char* address = nullptr;
    std::vector<unsigned char> signature;
    if (!load_point(path, section, point_name.c_str(), module, address, signature)) return false;
    output.emplace(std::move(dot_name), reinterpret_cast<DotTick>(address));
  }
  return !output.empty();
}

bool load_dot_predicates(const wchar_t* path, std::wstring_view section,
    unsigned char* module, std::unordered_map<std::string, HasDot>& output) {
  constexpr std::wstring_view prefix = L"actor_has_";
  constexpr std::wstring_view suffix = L"_rva";
  for (const auto& [key, ignored] : load_section(path, section)) {
    if (!key.starts_with(prefix) || !key.ends_with(suffix)) continue;
    const std::wstring point_name = key.substr(0, key.size() - suffix.size());
    const std::wstring_view wide_dot_name = std::wstring_view(key).substr(
        prefix.size(), key.size() - prefix.size() - suffix.size());
    std::string dot_name;
    if (!ascii_identifier(wide_dot_name, dot_name)) return false;
    unsigned char* address = nullptr;
    std::vector<unsigned char> signature;
    if (!load_point(path, section, point_name.c_str(), module, address, signature)) return false;
    output.emplace(std::move(dot_name), reinterpret_cast<HasDot>(address));
  }
  return !output.empty();
}

void write_absolute_jump(unsigned char* output, const void* destination) {
  output[0] = 0x48;
  output[1] = 0xB8;
  const auto address = reinterpret_cast<std::uintptr_t>(destination);
  std::memcpy(output + 2, &address, sizeof(address));
  output[10] = 0xFF;
  output[11] = 0xE0;
}

bool patch_target(Hook& hook, unsigned char* target, const void* replacement,
    const unsigned char* trampoline_prefix, size_t prefix_size,
    size_t patch_size, const void* continuation) {
  if (patch_size < kPatchSize || patch_size > hook.original.size()) return false;
  hook.target = target;
  hook.patch_size = patch_size;
  std::memcpy(hook.original.data(), target, patch_size);
  const size_t trampoline_size = prefix_size + kPatchSize;
  auto* trampoline = static_cast<unsigned char*>(VirtualAlloc(nullptr, trampoline_size,
      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  if (!trampoline) return false;
  std::memcpy(trampoline, trampoline_prefix, prefix_size);
  write_absolute_jump(trampoline + prefix_size, continuation);
  DWORD ignored = 0;
  if (!VirtualProtect(trampoline, trampoline_size, PAGE_EXECUTE_READ, &ignored)) {
    VirtualFree(trampoline, 0, MEM_RELEASE);
    return false;
  }

  std::vector<unsigned char> patch(patch_size, 0x90);
  write_absolute_jump(patch.data(), replacement);
  DWORD old_protection = 0;
  if (!VirtualProtect(target, patch.size(), PAGE_EXECUTE_READWRITE, &old_protection)) {
    VirtualFree(trampoline, 0, MEM_RELEASE);
    return false;
  }
  std::memcpy(target, patch.data(), patch.size());
  FlushInstructionCache(GetCurrentProcess(), target, patch.size());
  VirtualProtect(target, patch.size(), old_protection, &ignored);
  hook.trampoline = trampoline;
  return true;
}

void restore(Hook& hook) noexcept {
  if (!hook.target || !hook.trampoline) return;
  DWORD old_protection = 0;
  if (VirtualProtect(hook.target, hook.patch_size, PAGE_EXECUTE_READWRITE, &old_protection)) {
    std::memcpy(hook.target, hook.original.data(), hook.patch_size);
    FlushInstructionCache(GetCurrentProcess(), hook.target, hook.patch_size);
    DWORD ignored = 0;
    VirtualProtect(hook.target, hook.patch_size, old_protection, &ignored);
  }
  VirtualFree(hook.trampoline, 0, MEM_RELEASE);
  hook = {};
}

} // namespace

bool actor_has_buff_impl(void* actor, std::string_view stat_name,
    const double* amount, double tolerance) noexcept {
  const auto stat = g_layout.stats.find(std::string(stat_name));
  if (!actor || stat == g_layout.stats.end() || !g_layout.buff_entry_size ||
      tolerance < 0.0) {
    return false;
  }
  const auto base = reinterpret_cast<std::uintptr_t>(actor);
  const auto* begin_address = reinterpret_cast<const std::uintptr_t*>(
      base + g_layout.actor_buff_begin);
  const auto* end_address = reinterpret_cast<const std::uintptr_t*>(
      base + g_layout.actor_buff_end);
  if (!readable_range(begin_address, sizeof(*begin_address)) ||
      !readable_range(end_address, sizeof(*end_address))) return false;
  const auto begin = *begin_address;
  const auto end = *end_address;
  if (!begin || end < begin || (end - begin) % g_layout.buff_entry_size != 0 ||
      end - begin > g_layout.buff_entry_size * 1024 ||
      !readable_range(reinterpret_cast<const void*>(begin),
          static_cast<size_t>(end - begin))) {
    return false;
  }
  probe_actor_buffs(actor, stat_name);
  for (std::uintptr_t entry = begin; entry < end; entry += g_layout.buff_entry_size) {
    if (*reinterpret_cast<const std::uint32_t*>(entry + g_layout.buff_stat_type) !=
        stat->second) continue;
    if (!amount) return true;
    const float existing = *reinterpret_cast<const float*>(entry + g_layout.buff_amount);
    if (std::fabs(static_cast<double>(existing) - *amount) <= tolerance) {
      return true;
    }
  }
  return false;
}

bool actor_has_buff(void* actor, std::string_view stat_name) noexcept {
  return actor_has_buff_impl(actor, stat_name, nullptr, 0.0);
}

bool actor_has_buff(void* actor, std::string_view stat_name, double amount,
    double tolerance) noexcept {
  return actor_has_buff_impl(actor, stat_name, &amount, tolerance);
}

bool actor_has_buff_id(void* actor, std::string_view buff_id) noexcept {
  if (!actor || buff_id.empty() || buff_id.size() >= g_layout.buff_id_capacity ||
      !g_layout.buff_entry_size) return false;
  const auto base = reinterpret_cast<std::uintptr_t>(actor);
  const auto* begin_address = reinterpret_cast<const std::uintptr_t*>(
      base + g_layout.actor_buff_begin);
  const auto* end_address = reinterpret_cast<const std::uintptr_t*>(
      base + g_layout.actor_buff_end);
  if (!readable_range(begin_address, sizeof(*begin_address)) ||
      !readable_range(end_address, sizeof(*end_address))) return false;
  const auto begin = *begin_address;
  const auto end = *end_address;
  if (!begin || end < begin || (end - begin) % g_layout.buff_entry_size != 0 ||
      end - begin > g_layout.buff_entry_size * 1024 ||
      !readable_range(reinterpret_cast<const void*>(begin),
          static_cast<size_t>(end - begin))) return false;
  for (std::uintptr_t entry = begin; entry < end; entry += g_layout.buff_entry_size) {
    const char* existing = reinterpret_cast<const char*>(entry + g_layout.buff_id);
    const size_t length = strnlen_s(existing, g_layout.buff_id_capacity);
    if (length == buff_id.size() &&
        std::memcmp(existing, buff_id.data(), length) == 0) return true;
  }
  return false;
}

bool collect_actors(void* owner, std::string_view group_name,
    std::vector<void*>& actors) noexcept {
  actors.clear();
  if (!owner) return false;
  size_t begin_offset = 0;
  size_t end_offset = 0;
  if (group_name == "heroes") {
    begin_offset = g_layout.hero_actor_begin;
    end_offset = g_layout.hero_actor_end;
  } else if (group_name == "enemies") {
    begin_offset = g_layout.enemy_actor_begin;
    end_offset = g_layout.enemy_actor_end;
  } else {
    return false;
  }
  const auto base = reinterpret_cast<std::uintptr_t>(owner);
  const auto* begin_address = reinterpret_cast<const std::uintptr_t*>(base + begin_offset);
  const auto* end_address = reinterpret_cast<const std::uintptr_t*>(base + end_offset);
  if (!readable_range(begin_address, sizeof(*begin_address)) ||
      !readable_range(end_address, sizeof(*end_address))) return false;
  const auto begin = *begin_address;
  const auto end = *end_address;
  if (!begin || end < begin || (end - begin) % sizeof(void*) != 0 ||
      end - begin > sizeof(void*) * 16 || !readable_range(
          reinterpret_cast<const void*>(begin), static_cast<size_t>(end - begin))) {
    return false;
  }
  actors.reserve(static_cast<size_t>((end - begin) / sizeof(void*)));
  for (std::uintptr_t entry = begin; entry < end; entry += sizeof(void*)) {
    void* actor = *reinterpret_cast<void* const*>(entry);
    if (actor) actors.push_back(actor);
  }
  return true;
}

bool trigger_dot(std::string_view dot_name, void* battle, void* actor) noexcept {
  const auto dot = g_dot_ticks.find(std::string(dot_name));
  const auto predicate = g_dot_predicates.find(std::string(dot_name));
  if (dot == g_dot_ticks.end() || !dot->second || predicate == g_dot_predicates.end() ||
      !predicate->second || !battle || !actor) {
    return false;
  }
  if (!predicate->second(actor)) {
    return false;
  }
  dot->second(battle, actor);
  return true;
}

bool defer_dot(std::string_view dot_name, void* battle, void* actor) noexcept {
  const auto dot = g_dot_ticks.find(std::string(dot_name));
  const auto predicate = g_dot_predicates.find(std::string(dot_name));
  if (dot == g_dot_ticks.end() || !dot->second || predicate == g_dot_predicates.end() ||
      !predicate->second || !battle || !actor) {
    return false;
  }

  bool accepted = false;
  AcquireSRWLockExclusive(&g_deferred_lock);
  const bool duplicate = std::any_of(g_deferred_dots.begin(), g_deferred_dots.end(),
      [&](const DeferredDot& request) {
        return request.battle == battle && request.actor == actor &&
            request.name == dot_name;
      });
  if (duplicate) {
    accepted = true;
  } else if (g_deferred_dots.size() < kMaximumDeferredDots) {
    g_deferred_dots.push_back({std::string(dot_name), battle, actor});
    accepted = true;
  }
  ReleaseSRWLockExclusive(&g_deferred_lock);
  return accepted;
}

void __fastcall flush_deferred_dots(void* battle) noexcept {
  std::vector<DeferredDot> ready;
  AcquireSRWLockExclusive(&g_deferred_lock);
  for (auto it = g_deferred_dots.begin(); it != g_deferred_dots.end();) {
    if (it->battle == battle) {
      ready.push_back(std::move(*it));
      it = g_deferred_dots.erase(it);
    } else {
      ++it;
    }
  }
  ReleaseSRWLockExclusive(&g_deferred_lock);

  for (const DeferredDot& request : ready) {
    const auto dot = g_dot_ticks.find(request.name);
    const auto predicate = g_dot_predicates.find(request.name);
    if (dot == g_dot_ticks.end() || predicate == g_dot_predicates.end() ||
        !dot->second || !predicate->second || !predicate->second(request.actor)) {
      continue;
    }
    dot->second(request.battle, request.actor);
  }
}

bool install(std::wstring_view build_id, const wchar_t* profile_path,
    bool enable_buff_id_probe, bool enable_corridor_bridge,
    HostCallbacks callbacks, LogCallback log) noexcept {
  g_log = log;
  g_buff_id_probe_enabled = enable_buff_id_probe;
  g_callbacks = callbacks;
  auto* module = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
  if (!module || build_id.empty() || !profile_path || !callbacks.is_effect_observed ||
      !callbacks.before_effect_target || !callbacks.after_effect_target ||
      (enable_corridor_bridge && !callbacks.corridor_return_roll)) return false;

  unsigned char* named = nullptr;
  unsigned char* apply = nullptr;
  unsigned char* state_transition = nullptr;
  unsigned char* corridor_return = nullptr;
  std::vector<unsigned char> named_signature;
  std::vector<unsigned char> apply_signature;
  std::vector<unsigned char> state_transition_signature;
  std::vector<unsigned char> corridor_return_signature;
  Layout layout;
  std::unordered_map<std::string, DotTick> dot_ticks;
  std::unordered_map<std::string, HasDot> dot_predicates;
  if (!load_point(profile_path, build_id, L"named_effect_dispatch", module, named, named_signature) ||
      !load_point(profile_path, build_id, L"resolved_effect_apply", module, apply, apply_signature) ||
      !load_point(profile_path, build_id, L"battle_state_transition", module,
          state_transition, state_transition_signature) ||
      (enable_corridor_bridge && !load_point(profile_path, build_id,
          L"corridor_return_try_place", module, corridor_return,
          corridor_return_signature)) ||
      !load_layout(profile_path, build_id, layout) ||
      !load_dot_ticks(profile_path, build_id, module, dot_ticks) ||
      !load_dot_predicates(profile_path, build_id, module, dot_predicates)) {
    report("ERROR", "Game bridge profile missing or signature mismatch; bridge disabled (fail closed)");
    return false;
  }
  constexpr size_t state_transition_patch_size = 17;
  if (named_signature.size() < kPatchSize || apply_signature.size() < kPatchSize ||
      state_transition_signature.size() < state_transition_patch_size ||
      (enable_corridor_bridge && corridor_return_signature.size() < kPatchSize)) {
    report("ERROR", "Game bridge signatures are shorter than the required patch; bridge disabled");
    return false;
  }

  if (!patch_target(g_apply_hook, apply, reinterpret_cast<const void*>(&hooked_apply),
      apply_signature.data(), kPatchSize, kPatchSize, apply + kPatchSize)) {
    report("ERROR", "Could not install per-target effect bridge");
    return false;
  }
  g_original_apply = reinterpret_cast<ApplyEffect>(g_apply_hook.trampoline);

  std::array<unsigned char, 20> named_prefix{
      0x48, 0x85, 0xD2, 0x75, 0x0C,
      0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0,
      0x55, 0x53, 0x56};
  const auto original_return = reinterpret_cast<std::uintptr_t>(named + 0x2D2);
  std::memcpy(named_prefix.data() + 7, &original_return, sizeof(original_return));
  if (!patch_target(g_named_hook, named, reinterpret_cast<const void*>(&hooked_named),
      named_prefix.data(), named_prefix.size(), kPatchSize, named + kPatchSize)) {
    restore(g_apply_hook);
    g_original_apply = nullptr;
    report("ERROR", "Could not install named-effect bridge; earlier patch restored");
    return false;
  }
  g_original_named = reinterpret_cast<NamedEffect>(g_named_hook.trampoline);
  g_dot_ticks = std::move(dot_ticks);
  g_dot_predicates = std::move(dot_predicates);
  g_layout = layout;
  if (!patch_target(g_state_transition_hook, state_transition,
      reinterpret_cast<const void*>(&hooked_state_transition),
      state_transition_signature.data(), state_transition_patch_size,
      state_transition_patch_size, state_transition + state_transition_patch_size)) {
    restore(g_named_hook);
    restore(g_apply_hook);
    g_original_named = nullptr;
    g_original_apply = nullptr;
    g_dot_ticks.clear();
    g_dot_predicates.clear();
    g_layout = {};
    report("ERROR", "Could not install action-end bridge; earlier patches restored");
    return false;
  }
  g_original_state_transition = reinterpret_cast<StateTransition>(
      g_state_transition_hook.trampoline);
  g_module = module;
  if (enable_corridor_bridge) {
    if (!patch_target(g_corridor_return_hook, corridor_return,
        reinterpret_cast<const void*>(&hooked_corridor_return),
        corridor_return_signature.data(), kPatchSize, kPatchSize,
        corridor_return + kPatchSize)) {
      restore(g_state_transition_hook);
      restore(g_named_hook);
      restore(g_apply_hook);
      g_original_state_transition = nullptr;
      g_original_named = nullptr;
      g_original_apply = nullptr;
      g_dot_ticks.clear();
      g_dot_predicates.clear();
      g_layout = {};
      g_module = nullptr;
      report("ERROR", "Could not install corridor-return bridge; earlier patches restored");
      return false;
    }
    g_original_corridor_return = reinterpret_cast<ReturnTryPlace>(
        g_corridor_return_hook.trampoline);
  }
  report("INFO", enable_corridor_bridge
      ? "Generic Lua effect, action-end, and corridor-return bridges installed"
      : "Generic Lua effect and action-end bridges installed; corridor tracer owns its hook");
  if (g_buff_id_probe_enabled) {
    report("WARN", "Targeted buff-ID probe enabled; matching active buff entries will be logged");
  }
  return true;
}

} // namespace ddlua::game_bridge
