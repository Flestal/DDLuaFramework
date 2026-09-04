#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

namespace ddlua::game_bridge {

using LogCallback = void (*)(std::string_view level, std::string_view message) noexcept;
using IsEffectObserved = bool (*)(std::string_view effect_name) noexcept;
using BeforeEffectTarget = bool (*)(std::string_view effect_name, void* battle,
    void* performer, void* target) noexcept;
using AfterEffectTarget = void (*)(std::string_view effect_name, void* battle,
    void* performer, void* target) noexcept;

struct CorridorModifiers {
  double chance_multiplier_delta = 0.0;
  unsigned int extra_rolls = 0;
};

struct HostCallbacks {
  IsEffectObserved is_effect_observed = nullptr;
  BeforeEffectTarget before_effect_target = nullptr;
  AfterEffectTarget after_effect_target = nullptr;
  CorridorModifiers (*corridor_return_roll)(std::string_view content,
      double native_chance, double torchlight, void* raid) noexcept = nullptr;
};

// Installs build-verified, policy-free engine hooks. Effect names and behavior
// are supplied by Lua; this layer owns only native ABI and address validation.
bool install(std::wstring_view build_id, const wchar_t* profile_path,
    bool enable_buff_id_probe, bool enable_corridor_bridge,
    HostCallbacks callbacks, LogCallback log) noexcept;

bool actor_has_buff(void* actor, std::string_view stat_name) noexcept;
bool actor_has_buff(void* actor, std::string_view stat_name, double amount,
    double tolerance) noexcept;
bool actor_has_buff_id(void* actor, std::string_view buff_id) noexcept;
bool collect_actors(void* owner, std::string_view group_name,
    std::vector<void*>& actors) noexcept;
bool trigger_dot(std::string_view dot_name, void* battle, void* actor) noexcept;
bool defer_dot(std::string_view dot_name, void* battle, void* actor) noexcept;

} // namespace ddlua::game_bridge
