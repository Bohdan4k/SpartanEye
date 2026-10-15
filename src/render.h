#pragma once
#include <cstddef>

// D3D12 Present hook + Dear ImGui on the game's own command queue.
bool render_create_hooks(char* err, size_t errlen);  // dummy swap chain -> MinHook hooks (not enabled)
void render_queue_fallback_off();                  // MH_QueueDisableHook for the ExecuteCommandLists fallback
void render_shutdown();                              // after hooks are disabled and no thread is inside them
