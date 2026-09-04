#include "render_hook.hpp"

#include <windows.h>
#include <winnt.h>

#include <cstring>

namespace ddlua::render_hook {
namespace {

using SwapWindow = void(__cdecl*)(void* window);
SwapWindow g_original_swap = nullptr;
TickCallback g_tick_callback = nullptr;

void __cdecl hooked_swap_window(void* window) {
  g_original_swap(window);
  if (g_tick_callback) g_tick_callback();
}

} // namespace

bool install(TickCallback callback) noexcept {
  auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
  if (!base) return false;
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
  const auto& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!imports.VirtualAddress || !imports.Size) return false;

  auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
  for (; descriptor->Name; ++descriptor) {
    const char* module_name = reinterpret_cast<const char*>(base + descriptor->Name);
    if (_stricmp(module_name, "SDL2.dll") != 0) continue;
    if (!descriptor->OriginalFirstThunk || !descriptor->FirstThunk) return false;
    auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->OriginalFirstThunk);
    auto* addresses = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->FirstThunk);
    for (; names->u1.AddressOfData; ++names, ++addresses) {
      if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
      auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
      if (std::strcmp(reinterpret_cast<const char*>(import->Name), "SDL_GL_SwapWindow") != 0) continue;
      DWORD old_protection = 0;
      if (!VirtualProtect(&addresses->u1.Function, sizeof(addresses->u1.Function), PAGE_READWRITE, &old_protection)) return false;
      g_original_swap = reinterpret_cast<SwapWindow>(addresses->u1.Function);
      g_tick_callback = callback;
      addresses->u1.Function = reinterpret_cast<ULONGLONG>(&hooked_swap_window);
      DWORD ignored = 0;
      VirtualProtect(&addresses->u1.Function, sizeof(addresses->u1.Function), old_protection, &ignored);
      return g_original_swap != nullptr;
    }
  }
  return false;
}

} // namespace ddlua::render_hook

