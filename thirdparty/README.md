# Vendored third-party code

Unmodified files copied from the official GitHub repositories at the release tags below (approved by the user,
2026-10-13). Only the files the overlay compiles are kept.

| Library | Version | Source | Commit (= GitHub API tag ref) | License |
|---|---|---|---|---|
| MinHook | v1.3.4 (2025-03-28) | https://github.com/TsudaKageyu/minhook | `c3fcafdc10146beb5919319d0683e44e3c30d537` | BSD-2-Clause (`minhook/LICENSE.txt`) |
| Dear ImGui | v1.92.9b (2026-07-31) | https://github.com/ocornut/imgui | `f1cc2ae15e53a861a874c3034aae6798fde194ab` | MIT (`imgui/LICENSE.txt`) |

Fetched with `git clone --depth 1 --branch <tag>` into an empty scratch folder; `git rev-parse HEAD` matched the
commit that `api.github.com/repos/<repo>/git/ref/tags/<tag>` reports.

Kept files:
- MinHook: `include/MinHook.h`, `src/{buffer,hook,trampoline}.c`, `src/{buffer,trampoline}.h`,
  `src/hde/{hde64.c,hde64.h,table64.h,pstdint.h}` (x64 only; `hde32` dropped).
- ImGui: core (`imgui*.cpp/h`, `imconfig.h`, `imstb_*.h`) + `backends/imgui_impl_{dx12,win32}.{cpp,h}`.
  Not kept: demo, examples, misc, other backends.

## Review summary (2026-10-13)
- **MinHook** (~1.9k lines): read `hook.c` thread freezing and `MH_CreateHookApiEx`. OS calls: `VirtualAlloc/Protect/
  Query/Free` (trampoline buffers near the target), `FlushInstructionCache`, `Heap*`, Toolhelp thread snapshot filtered
  to **its own process** (`th32OwnerProcessID == GetCurrentProcessId()`), `Suspend/ResumeThread`, `Get/SetThreadContext`
  (moves frozen threads' IPs out of patched bytes), `GetModuleHandleW` + `GetProcAddress` (already-loaded modules only).
  No file, registry, network or process-creation code.
- **Dear ImGui** (~59k lines incl. backends): grepped every OS call. Present and **compiled out** by our defines:
  file I/O (`imgui.ini` / log via `ImFileOpen`, `_wfopen`) → `IMGUI_DISABLE_FILE_FUNCTIONS`; Win32 clipboard →
  `IMGUI_DISABLE_WIN32_DEFAULT_CLIPBOARD_FUNCTIONS`; `ShellExecuteW` (open URL) → `IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS`;
  XInput DLL loading in the Win32 backend → `IMGUI_IMPL_WIN32_DISABLE_GAMEPAD`. Remaining:
  - DX12 backend: `D3DCompile` (system `d3dcompiler_47.dll`) for its two tiny shaders; `GetModuleHandleA("d3d12.dll")`
    (its fallback `LoadLibraryA(".\\d3d12.dll")` only runs when d3d12 is not loaded, never the case in this game).
  - Win32 backend: `GetProcAddress` for DPI / version helpers on user32/shcore/ntdll (we don't call the DPI helpers).
  No network, registry or process-creation code. No window hooks are installed by the backends (we never call
  `ImGui_ImplWin32_WndProcHandler`; input comes from our DirectInput hooks).
