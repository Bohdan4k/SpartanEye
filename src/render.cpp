#include "render.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <vector>

#include "MinHook.h"
#include "game.h"
#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "input.h"
#include "capture.h"
#include "config.h"
#include "log.h"
#include "report.h"
#include "state.h"
#include "ui.h"
#include "version.h"

namespace
{
using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ExecuteCommandLists_t = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

constexpr int kVtPresent = 8;              // IDXGISwapChain::Present
constexpr int kVtPresent1 = 22;            // IDXGISwapChain1::Present1
constexpr int kVtExecuteCommandLists = 10; // ID3D12CommandQueue::ExecuteCommandLists
constexpr UINT kSrvHeapSize = 64;
constexpr UINT kMaxBuffers = 8;

Present_t o_present = nullptr;
Present1_t o_present1 = nullptr;
ExecuteCommandLists_t o_ecl = nullptr;
void* t_ecl = nullptr;
std::atomic<ID3D12CommandQueue*> g_seen_queue{nullptr};  // fallback: last DIRECT queue seen in ExecuteCommandLists
thread_local ID3D12CommandQueue* tl_seen_queue = nullptr;
thread_local bool tl_in_present = false;

struct FrameCtx
{
    ID3D12CommandAllocator* alloc = nullptr;
    UINT64 fence_value = 0;
};

struct D3D
{
    IDXGISwapChain3* sc = nullptr;  // the game's swap chain (no reference held, see init)
    HWND hwnd = nullptr;
    ID3D12Device* dev = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    UINT rtv_inc = 0;
    ID3D12DescriptorHeap* srv_heap = nullptr;
    UINT srv_inc = 0;
    std::vector<int> srv_free;
    ID3D12GraphicsCommandList* cl = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fence_event = nullptr;
    UINT64 fence_counter = 0;
    std::vector<FrameCtx> frames;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    UINT buffers = 0;
    bool imgui_ctx = false;
    bool imgui_dx12 = false;
    bool failed = false;
    bool waiting_queue_logged = false;
    UINT style_height = 0;  // buffer height the ImGui style was scaled for
    LARGE_INTEGER qpf{}, last{};
    ImVec2 cursor{-1, -1};
} g;

template <class T>
void safe_release(T*& p)
{
    if (p)
    {
        p->Release();
        p = nullptr;
    }
}

void wait_fence(UINT64 v)
{
    if (!g.fence || !v || g.fence->GetCompletedValue() >= v)
        return;
    g.fence->SetEventOnCompletion(v, g.fence_event);
    WaitForSingleObject(g.fence_event, 2000);
}

void wait_idle() { wait_fence(g.fence_counter); }

void srv_alloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
{
    IM_ASSERT(!g.srv_free.empty());
    int i = g.srv_free.back();
    g.srv_free.pop_back();
    cpu->ptr = g.srv_heap->GetCPUDescriptorHandleForHeapStart().ptr + static_cast<SIZE_T>(i) * g.srv_inc;
    gpu->ptr = g.srv_heap->GetGPUDescriptorHandleForHeapStart().ptr + static_cast<UINT64>(i) * g.srv_inc;
}

void srv_free(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE)
{
    g.srv_free.push_back(static_cast<int>((cpu.ptr - g.srv_heap->GetCPUDescriptorHandleForHeapStart().ptr) / g.srv_inc));
}

// Device objects that depend on the swap chain's buffer count / format.
void destroy_device_objects()
{
    wait_idle();
    if (g.imgui_dx12)
    {
        ImGui_ImplDX12_Shutdown();
        g.imgui_dx12 = false;
    }
    for (auto& f : g.frames)
        safe_release(f.alloc);
    g.frames.clear();
    safe_release(g.cl);
    safe_release(g.rtv_heap);
    safe_release(g.srv_heap);
}

bool create_device_objects(UINT buffers, DXGI_FORMAT fmt)
{
    auto fail = [](const char* what, HRESULT hr) {
        logf("D3D12: %s failed (hr=0x%08lX); overlay rendering disabled", what, hr);
        g.failed = true;
        return false;
    };
    HRESULT hr;
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = kMaxBuffers;
    if (FAILED(hr = g.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.rtv_heap))))
        return fail("CreateDescriptorHeap(RTV)", hr);
    g.rtv_inc = g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kSrvHeapSize;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(hr = g.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.srv_heap))))
        return fail("CreateDescriptorHeap(SRV)", hr);
    g.srv_inc = g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g.srv_free.clear();
    for (int i = kSrvHeapSize - 1; i >= 0; --i)
        g.srv_free.push_back(i);
    g.frames.resize(buffers);
    for (auto& f : g.frames)
        if (FAILED(hr = g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.alloc))))
            return fail("CreateCommandAllocator", hr);
    if (FAILED(hr = g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.frames[0].alloc, nullptr,
                                             IID_PPV_ARGS(&g.cl))))
        return fail("CreateCommandList", hr);
    g.cl->Close();

    ImGui_ImplDX12_InitInfo ii;
    ii.Device = g.dev;
    ii.CommandQueue = g.queue;
    ii.NumFramesInFlight = static_cast<int>(buffers);
    ii.RTVFormat = fmt;
    ii.DSVFormat = DXGI_FORMAT_UNKNOWN;
    ii.SrvDescriptorHeap = g.srv_heap;
    ii.SrvDescriptorAllocFn = srv_alloc;
    ii.SrvDescriptorFreeFn = srv_free;
    if (!ImGui_ImplDX12_Init(&ii))
        return fail("ImGui_ImplDX12_Init", E_FAIL);
    g.imgui_dx12 = true;
    g.buffers = buffers;
    g.fmt = fmt;
    logf("D3D12: device objects ready (%u buffers, format %d)", buffers, static_cast<int>(fmt));
    return true;
}

bool find_queue()
{
    if (g.queue)
        return true;
    // 1) A D3D12 swap chain is created on a command queue; DXGI hands it back through GetDevice.
    ID3D12CommandQueue* q = nullptr;
    if (SUCCEEDED(g.sc->GetDevice(IID_PPV_ARGS(&q))) && q)
    {
        D3D12_COMMAND_QUEUE_DESC d = q->GetDesc();
        if (d.Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
        {
            g.queue = q;
            logf("D3D12: command queue %p via IDXGISwapChain::GetDevice", q);
            return true;
        }
        q->Release();
    }
    // 2) Fallback: the last DIRECT queue the presenting thread executed on (ExecuteCommandLists hook).
    if (t_ecl && !o_ecl)
        return false;
    if (t_ecl && !g.waiting_queue_logged)
    {
        g.waiting_queue_logged = true;
        MH_EnableHook(t_ecl);
        logf("D3D12: GetDevice gave no queue; watching ExecuteCommandLists");
        return false;
    }
    q = tl_seen_queue ? tl_seen_queue : g_seen_queue.load();
    if (!q)
        return false;
    q->AddRef();
    g.queue = q;
    MH_DisableHook(t_ecl);
    logf("D3D12: command queue %p via ExecuteCommandLists", q);
    return true;
}

bool ensure_init(IDXGISwapChain* sc0)
{
    if (g.failed)
        return false;
    if (!g.sc)
    {
        IDXGISwapChain3* sc = nullptr;
        if (FAILED(sc0->QueryInterface(IID_PPV_ARGS(&sc))))
            return false;
        // Hold no reference: an extra ref would stop the game from recreating its swap chain for this window.
        sc->Release();
        ID3D12Device* dev = nullptr;
        if (FAILED(sc->GetDevice(IID_PPV_ARGS(&dev))))
            return false;  // not a D3D12 swap chain
        g.sc = sc;
        g.dev = dev;
        sc->GetHwnd(&g.hwnd);
        if (FAILED(g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence))))
        {
            logf("D3D12: CreateFence failed");
            g.failed = true;
            return false;
        }
        g.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        QueryPerformanceFrequency(&g.qpf);
        QueryPerformanceCounter(&g.last);
        DXGI_SWAP_CHAIN_DESC1 d{};
        sc->GetDesc1(&d);
        logf("D3D12: swap chain %p hwnd %p %ux%u format %d buffers %u", sc, g.hwnd, d.Width, d.Height,
             static_cast<int>(d.Format), d.BufferCount);
    }
    else if (sc0 != g.sc)
    {
        IDXGISwapChain3* sc = nullptr;
        if (FAILED(sc0->QueryInterface(IID_PPV_ARGS(&sc))))
            return false;
        sc->Release();
        if (sc != g.sc)
        {
            HWND h = nullptr;
            sc->GetHwnd(&h);
            // Another window's swap chain (a tool, an overlay): ignore, unless the game's old window is gone (some
            // display-mode switches recreate the window) and the new one belongs to this process.
            DWORD owner = 0;
            if (h != g.hwnd && (IsWindow(g.hwnd) || !h || !GetWindowThreadProcessId(h, &owner) ||
                                owner != GetCurrentProcessId()))
                return false;
            ID3D12Device* dev = nullptr;
            if (FAILED(sc->GetDevice(IID_PPV_ARGS(&dev))))
                return false;
            dev->Release();
            if (dev != g.dev)
            {
                // A new device (device lost / GPU switch): our queue and objects belong to the old one. Stop cleanly.
                logf("D3D12: the game recreated its device; overlay rendering stops (re-inject to continue)");
                g.failed = true;
                return false;
            }
            logf("D3D12: game swap chain recreated (%p -> %p, window %p -> %p)", g.sc, sc, g.hwnd, h);
            destroy_device_objects();
            g.sc = sc;
            g.hwnd = h;
        }
    }
    if (!find_queue())
        return false;
    if (!g.imgui_ctx)
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;  // never write imgui.ini (the game's working dir is its install folder)
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        io.BackendPlatformName = "metro_npc_overlay";
        ImGui::StyleColorsDark();
        ui_setup_fonts();
        g.imgui_ctx = true;
    }
    DXGI_SWAP_CHAIN_DESC1 d{};
    g.sc->GetDesc1(&d);
    if (!g.imgui_dx12 || d.Format != g.fmt || d.BufferCount != g.buffers)
    {
        if (g.imgui_dx12)
            destroy_device_objects();
        if (d.BufferCount == 0 || d.BufferCount > kMaxBuffers || !create_device_objects(d.BufferCount, d.Format))
        {
            g.failed = true;
            return false;
        }
        g.style_height = 0;
    }
    if (d.Height != g.style_height && d.Height > 0)  // first frame or a resolution change: UI size follows
    {
        g.style_height = d.Height;
        float scale = d.Height / 1080.0f;
        scale = scale < 1.0f ? 1.0f : (scale > 2.5f ? 2.5f : scale);
        ImGuiStyle& st = ImGui::GetStyle();
        st = ImGuiStyle();
        ImGui::StyleColorsDark(&st);
        st.ScaleAllSizes(scale);
        st.FontScaleMain = scale;
    }
    return true;
}

// vw x vh: the game's viewport (top-left of the buffer); bw x bh: the swap-chain buffer.
void submit(UINT vw, UINT vh, UINT bw, UINT bh)
{
    UINT idx = g.sc->GetCurrentBackBufferIndex();
    FrameCtx& f = g.frames[idx % g.frames.size()];
    wait_fence(f.fence_value);  // this allocator's last use
    // ImGui rotates its vertex/index buffers by its own render count: keep at most N-1 of our submissions in flight.
    if (g.fence_counter + 1 > g.buffers)
        wait_fence(g.fence_counter + 1 - g.buffers);
    ID3D12Resource* rt = nullptr;
    if (FAILED(g.sc->GetBuffer(idx, IID_PPV_ARGS(&rt))))
        return;
    f.alloc->Reset();
    g.cl->Reset(f.alloc, nullptr);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(idx % kMaxBuffers) * g.rtv_inc;
    g.dev->CreateRenderTargetView(rt, nullptr, rtv);

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = rt;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g.cl->ResourceBarrier(1, &b);
    g.cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    if (vh < bh || vw < bw)
    {
        // The game never draws outside its viewport (that area is black before we are loaded); clear it so nothing of
        // ours can linger there.
        const FLOAT black[4] = {0, 0, 0, 1};
        D3D12_RECT rects[2];
        UINT n = 0;
        if (vh < bh)
            rects[n++] = {0, static_cast<LONG>(vh), static_cast<LONG>(bw), static_cast<LONG>(bh)};
        if (vw < bw)
            rects[n++] = {static_cast<LONG>(vw), 0, static_cast<LONG>(bw), static_cast<LONG>(vh)};
        g.cl->ClearRenderTargetView(rtv, black, n, rects);
    }
    g.cl->SetDescriptorHeaps(1, &g.srv_heap);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g.cl);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g.cl->ResourceBarrier(1, &b);
    g.cl->Close();
    ID3D12CommandList* lists[] = {g.cl};
    g.queue->ExecuteCommandLists(1, lists);
    g.queue->Signal(g.fence, ++g.fence_counter);
    f.fence_value = g.fence_counter;
    rt->Release();  // the swap chain keeps the buffer alive; we keep no long-lived reference
}

// Frame capture for offline previews (capture.h): on the named event, write the frame data next to the DLL and draw
// nothing for a few frames. Returns true while the overlay must stay hidden.
bool capture_poll(const FrameData& fd)
{
    static HANDLE ev = nullptr;
    static int hide = 0, seq = 0;
    if (!ev)
    {
        wchar_t name[64];
        swprintf_s(name, L"Local\\MetroNpcOverlay_Capture_%lu", GetCurrentProcessId());
        ev = CreateEventW(nullptr, FALSE, FALSE, name);  // auto-reset; lives as long as the process (one handle)
        if (!ev)
            return false;
    }
    if (hide > 0)
        return --hide, true;
    if (WaitForSingleObject(ev, 0) != WAIT_OBJECT_0)
        return false;
    DXGI_SWAP_CHAIN_DESC1 d{};
    g.sc->GetDesc1(&d);
    float w = static_cast<float>(d.Width), h = static_cast<float>(d.Height);
    RECT crc{};
    if (GetClientRect(g.hwnd, &crc) && crc.right > 0 && crc.bottom > 0)
    {
        w = fminf(w, static_cast<float>(crc.right));
        h = fminf(h, static_cast<float>(crc.bottom));
    }
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    swprintf_s(dir, L"%scaptures", log_dir());
    CreateDirectoryW(dir, nullptr);
    swprintf_s(path, L"%s\\frame_%03d.txt", dir, seq++);
    bool ok = capture_write(fd, w, h, path);
    logf("capture: frame data %s (%zu NPCs)", ok ? "written" : "FAILED", fd.npcs.size());
    hide = 12;
    return true;
}

void on_present(IDXGISwapChain* sc0)
{
    if (g_unload_requested.load() || !ensure_init(sc0))
        return;

    config_autosave();
    InputFrame in;
    input_poll(in, g.hwnd);
    if (in.end_pressed)
    {
        logf("END pressed: unloading");
        request_unload();
        return;
    }
    if (in.insert_pressed)
    {
        bool open = !g_menu_open.load();
        g_menu_open.store(open);
        g.cursor = ImVec2(-1, -1);  // re-centre on open
        logf("menu %s", open ? "opened" : "closed");
    }
    const bool menu = g_menu_open.load();
    static bool greeted = false;
    if (!greeted)
    {
        greeted = true;
        char msg[160];
        snprintf(msg, sizeof(msg), "%s %s loaded  -  %s: menu   %s: NPC report   %s: unload", MOD_TITLE,
                 kVersionString, config_key_name(g_settings.key_menu), config_key_name(g_settings.key_report),
                 config_key_name(g_settings.key_unload));
        ui_toast(msg, 6.f);
    }
    if (!menu && !g_settings.enabled && !ui_toast_active() && !in.report_pressed)
        return;  // overlay off: no game reads at all

    static FrameData fd;
    if (menu || g_settings.enabled || in.report_pressed)
        game_update(fd);
    if (capture_poll(fd))
        return;  // capture in progress: draw nothing so the clean game image can be grabbed
    if (in.report_pressed)
    {
        const float radius = g_settings.max_dist > 150.f ? g_settings.max_dist : 150.f;
        int n = report_write(fd, radius);
        char msg[160];
        if (n >= 0)
            snprintf(msg, sizeof(msg), "NPC report saved: %d NPCs within %.0f m  ->  metro_npc_reports.log", n, radius);
        else
            snprintf(msg, sizeof(msg), "NPC report failed (no level loaded?)");
        ui_toast(msg, 4.f);
    }
    const bool hidden = fd.paused && !g_settings.show_when_paused;
    if (!menu && !ui_toast_active() &&
        !(g_settings.enabled && fd.ok && !hidden && (!fd.npcs.empty() || !fd.zones.empty())))
        return;  // nothing to draw: no ImGui frame, no GPU work

    DXGI_SWAP_CHAIN_DESC1 d{};
    g.sc->GetDesc1(&d);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(d.Width), static_cast<float>(d.Height));
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = static_cast<float>(now.QuadPart - g.last.QuadPart) / static_cast<float>(g.qpf.QuadPart);
    io.DeltaTime = dt < 1e-4f ? 1e-4f : (dt > 0.1f ? 0.1f : dt);
    g.last = now;

    // The game renders at the window's client size into the top-left of its buffer. In windowed mode that can be
    // smaller than the buffer (2560x1421 inside 2560x1440 here) and DXGI stretches the whole buffer into the window,
    // so markers are projected to, and clipped to, the client-sized viewport.
    UINT vw = d.Width, vh = d.Height;
    RECT crc{};
    if (GetClientRect(g.hwnd, &crc) && crc.right > 0 && crc.bottom > 0)
    {
        vw = static_cast<UINT>(crc.right) < d.Width ? static_cast<UINT>(crc.right) : d.Width;
        vh = static_cast<UINT>(crc.bottom) < d.Height ? static_cast<UINT>(crc.bottom) : d.Height;
    }
    // Cross-check with the camera's own aspect (it is the viewport the game renders): a client rect scaled by DPI
    // virtualisation or caught mid-resize would otherwise misplace every marker. The camera wins when they disagree.
    if (fd.ok && fd.aspect > 0.5f && fd.aspect < 6.f &&
        fabsf(static_cast<float>(vw) / static_cast<float>(vh) / fd.aspect - 1.f) > 0.01f)
    {
        const float fit_h = static_cast<float>(d.Width) / fd.aspect;
        if (fit_h <= static_cast<float>(d.Height) + 0.5f)
            vw = d.Width, vh = static_cast<UINT>(fit_h + 0.5f);
        else
            vw = static_cast<UINT>(static_cast<float>(d.Height) * fd.aspect + 0.5f), vh = d.Height;
        vw = vw < 1 ? 1 : vw;
        vh = vh < 1 ? 1 : (vh > d.Height ? d.Height : vh);
    }
    static UINT logged_vw = 0, logged_vh = 0;
    if (vw != logged_vw || vh != logged_vh)
    {
        logged_vw = vw;
        logged_vh = vh;
        logf("viewport %ux%u in a %ux%u buffer (camera aspect %.4f, viewport aspect %.4f)", vw, vh, d.Width, d.Height,
             fd.aspect, static_cast<float>(vw) / vh);
    }
    const float fvw = static_cast<float>(vw), fvh = static_cast<float>(vh);

    io.MouseDrawCursor = menu;
    if (menu)
    {
        if (g.cursor.x < 0)
            g.cursor = ImVec2(fvw * 0.5f, fvh * 0.5f);
        if (in.di_mouse)
        {
            g.cursor.x += in.dx;
            g.cursor.y += in.dy;
        }
        else
        {
            POINT p;
            RECT rc;
            if (GetCursorPos(&p) && ScreenToClient(g.hwnd, &p) && GetClientRect(g.hwnd, &rc) && rc.right > 0 &&
                rc.bottom > 0)
                g.cursor = ImVec2(p.x * static_cast<float>(d.Width) / rc.right, p.y * static_cast<float>(d.Height) / rc.bottom);
        }
        g.cursor.x = g.cursor.x < 0 ? 0 : (g.cursor.x > fvw - 1.0f ? fvw - 1.0f : g.cursor.x);
        g.cursor.y = g.cursor.y < 0 ? 0 : (g.cursor.y > fvh - 1.0f ? fvh - 1.0f : g.cursor.y);
        io.AddMousePosEvent(g.cursor.x, g.cursor.y);
        io.AddMouseButtonEvent(0, in.lmb);
        io.AddMouseButtonEvent(1, in.rmb);
    }
    else
    {
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        io.AddMouseButtonEvent(0, false);
        io.AddMouseButtonEvent(1, false);
    }

    ImGui_ImplDX12_NewFrame();
    ImGui::NewFrame();
    ui_draw(fd, fvw, fvh, menu);
    ImGui::Render();
    submit(vw, vh, d.Width, d.Height);
}

// SEH net: a bug in our frame code disables the overlay instead of crashing the game.
void on_present_guarded(IDXGISwapChain* sc)
{
    __try
    {
        on_present(sc);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        g.failed = true;
        logf("exception 0x%08lX in the frame code; overlay rendering disabled", GetExceptionCode());
    }
}

// Frame-time cost of the overlay (CPU time of our frame code on the game's render thread), logged every 30 s next to
// the game's own frame time.
void on_present_timed(IDXGISwapChain* sc)
{
    static LARGE_INTEGER qpf{}, since{};
    static double sum_ms = 0, max_ms = 0;
    static int frames = 0;
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    if (!qpf.QuadPart)
    {
        QueryPerformanceFrequency(&qpf);
        since = t0;
    }
    on_present_guarded(sc);
    QueryPerformanceCounter(&t1);
    const double ms = 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(qpf.QuadPart);
    sum_ms += ms;
    max_ms = ms > max_ms ? ms : max_ms;
    ++frames;
    const double span = static_cast<double>(t1.QuadPart - since.QuadPart) / static_cast<double>(qpf.QuadPart);
    if (span >= 30.0)
    {
        logf("cost (%.0f s, %d frames): overlay %.3f ms/frame avg, %.2f ms max; game frame %.2f ms avg", span, frames,
             sum_ms / frames, max_ms, 1000.0 * span / frames);
        since = t1;
        sum_ms = max_ms = 0;
        frames = 0;
    }
}

HRESULT STDMETHODCALLTYPE hk_present(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    ActiveCall ac;
    const bool outer = !tl_in_present;
    if (outer)
    {
        tl_in_present = true;
        if (!(flags & DXGI_PRESENT_TEST))
            on_present_timed(sc);
    }
    HRESULT hr = o_present(sc, sync, flags);
    if (outer)
        tl_in_present = false;
    return hr;
}

HRESULT STDMETHODCALLTYPE hk_present1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p)
{
    ActiveCall ac;
    const bool outer = !tl_in_present;
    if (outer)
    {
        tl_in_present = true;
        if (!(flags & DXGI_PRESENT_TEST))
            on_present_timed(sc);
    }
    HRESULT hr = o_present1(sc, sync, flags, p);
    if (outer)
        tl_in_present = false;
    return hr;
}

void STDMETHODCALLTYPE hk_ecl(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists)
{
    ActiveCall ac;
    if (q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
    {
        tl_seen_queue = q;
        g_seen_queue.store(q);
    }
    o_ecl(q, n, lists);
}

LRESULT CALLBACK dummy_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }
}  // namespace

bool render_create_hooks(char* err, size_t errlen)
{
    HINSTANCE hinst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = dummy_wndproc;
    wc.hInstance = hinst;
    wc.lpszClassName = L"MetroNpcOverlayDummy";
    RegisterClassExW(&wc);
    HWND w = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, hinst,
                             nullptr);
    IDXGIFactory4* f = nullptr;
    IDXGIAdapter1* ad = nullptr;
    ID3D12Device* dev = nullptr;
    ID3D12CommandQueue* q = nullptr;
    IDXGISwapChain1* sc = nullptr;
    bool ok = false;
    do
    {
        if (!w || FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&f))))
        {
            snprintf(err, errlen, "dummy window / DXGI factory failed");
            break;
        }
        IDXGIFactory6* f6 = nullptr;
        if (SUCCEEDED(f->QueryInterface(IID_PPV_ARGS(&f6))))
        {
            f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&ad));
            f6->Release();
        }
        // D3D12 devices are per-adapter singletons: on the game's GPU this returns the game's own device.
        if (FAILED(D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))))
        {
            snprintf(err, errlen, "D3D12CreateDevice failed");
            break;
        }
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))))
        {
            snprintf(err, errlen, "CreateCommandQueue failed");
            break;
        }
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = 64;
        sd.Height = 64;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        if (FAILED(f->CreateSwapChainForHwnd(q, w, &sd, nullptr, nullptr, &sc)))
        {
            snprintf(err, errlen, "dummy CreateSwapChainForHwnd failed");
            break;
        }
        void** vt = *reinterpret_cast<void***>(sc);
        void** qvt = *reinterpret_cast<void***>(q);
        MH_STATUS s1 = MH_CreateHook(vt[kVtPresent], reinterpret_cast<void*>(&hk_present), reinterpret_cast<void**>(&o_present));
        MH_STATUS s2 = MH_CreateHook(vt[kVtPresent1], reinterpret_cast<void*>(&hk_present1), reinterpret_cast<void**>(&o_present1));
        t_ecl = qvt[kVtExecuteCommandLists];
        MH_STATUS s3 = MH_CreateHook(t_ecl, reinterpret_cast<void*>(&hk_ecl), reinterpret_cast<void**>(&o_ecl));
        logf("hooks: Present %p %s, Present1 %p %s, ExecuteCommandLists %p %s (enabled only if needed)", vt[kVtPresent],
             MH_StatusToString(s1), vt[kVtPresent1], MH_StatusToString(s2), t_ecl, MH_StatusToString(s3));
        if (s3 != MH_OK)
            t_ecl = nullptr;
        if (s1 != MH_OK || s2 != MH_OK)
        {
            snprintf(err, errlen, "MH_CreateHook(Present/Present1) failed");
            break;
        }
        ok = true;
    } while (false);
    safe_release(sc);
    safe_release(q);
    safe_release(dev);
    safe_release(ad);
    safe_release(f);
    if (w)
        DestroyWindow(w);
    UnregisterClassW(wc.lpszClassName, hinst);
    return ok;
}

void render_queue_fallback_off()
{
    if (t_ecl)
        MH_QueueDisableHook(t_ecl);
}

void render_shutdown()
{
    if (g.imgui_ctx || g.fence)
    {
        destroy_device_objects();
        if (g.imgui_ctx)
        {
            ImGui::DestroyContext();
            g.imgui_ctx = false;
        }
    }
    safe_release(g.fence);
    if (g.fence_event)
    {
        CloseHandle(g.fence_event);
        g.fence_event = nullptr;
    }
    safe_release(g.queue);
    safe_release(g.dev);
    g.sc = nullptr;
}
