// SN2ThirdPersonSettings v3.0
// True in-game ImGui overlay via D3D12/DXGI injection.
// Hooks IDXGIFactory2::CreateSwapChainForHwnd → IDXGISwapChain::Present.
// Captures UE's ID3D12CommandQueue via ExecuteCommandLists hook for proper sync.
// UE4SS console/tab NOT required; overlay always renders over the game.

#include <Mod/CppUserModBase.hpp>
#include <UE4SSProgram.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <imgui.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>

#include <MinHook.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <windows.h>
#include <dxgi1_4.h>
#include <d3d12.h>

// ── Settings ──────────────────────────────────────────────────────────────────

struct TPSettings
{
    bool        hideHUD   = true;
    std::string toggleKey = "F3";

    bool operator==(const TPSettings& o) const
    {
        return hideHUD == o.hideHUD && toggleKey == o.toggleKey;
    }
    bool operator!=(const TPSettings& o) const { return !(*this == o); }
};

// ── File I/O ──────────────────────────────────────────────────────────────────

static std::filesystem::path GetModRootDir()
{
    wchar_t buf[MAX_PATH]{};
    HMODULE hm = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&GetModRootDir), &hm);
    GetModuleFileNameW(hm, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path().parent_path();
}

static TPSettings LoadSettings()
{
    TPSettings s;
    std::ifstream f(GetModRootDir() / L"settings.ini");
    if (!f.is_open()) return s;

    std::string line;
    while (std::getline(f, line))
    {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (!val.empty() && val.back() == '\r') val.pop_back();
        if (key == "HideHUD")
            s.hideHUD = (val == "true" || val == "1");
        else if (key == "ToggleKey" && !val.empty())
            s.toggleKey = val;
    }
    return s;
}

static void SaveSettings(const TPSettings& s)
{
    std::ofstream f(GetModRootDir() / L"settings.ini");
    if (!f.is_open()) return;
    f << "HideHUD="   << (s.hideHUD ? "true" : "false") << "\n";
    f << "ToggleKey=" << s.toggleKey << "\n";
}

// ── Key presets ───────────────────────────────────────────────────────────────

static constexpr const char* k_keys[] = {
    "F1","F2","F3","F4","F5","F6","F7","F8","F9","F10","F11","F12",
    "F13","F14","F15","F16","F17","F18","F19","F20","F21","F22","F23","F24",
    "ONE","TWO","THREE","FOUR","FIVE","SIX","SEVEN","EIGHT","NINE","ZERO",
    "NUM_ONE","NUM_TWO","NUM_THREE","NUM_FOUR","NUM_FIVE",
    "NUM_SIX","NUM_SEVEN","NUM_EIGHT","NUM_NINE","NUM_ZERO",
    "A","B","C","D","E","F","G","H","I","J","K","L","M",
    "N","O","P","Q","R","S","T","U","V","W","X","Y","Z",
    "RETURN","BACKSPACE","TAB","ESCAPE","SPACE",
    "INSERT","DELETE","HOME","END","PAGE_UP","PAGE_DOWN",
    "UP_ARROW","DOWN_ARROW","LEFT_ARROW","RIGHT_ARROW",
    "LEFT_MOUSE_BUTTON","RIGHT_MOUSE_BUTTON","MIDDLE_MOUSE_BUTTON",
};
static constexpr int k_key_count = static_cast<int>(sizeof(k_keys) / sizeof(k_keys[0]));

static int FindKeyIndex(const std::string& key)
{
    for (int i = 0; i < k_key_count; ++i)
        if (key == k_keys[i]) return i;
    return -1;
}

// ── D3D12 overlay internals ───────────────────────────────────────────────────

static constexpr int NUM_BACK_BUFFERS = 3;

// Shared render state (written on render thread, read by hooks)
static std::mutex             g_overlayMtx;
static bool                   g_d3dReady          = false;
static HWND                   g_gameHwnd          = nullptr;

// D3D12 objects
static ID3D12Device*              g_device        = nullptr;
static ID3D12CommandQueue*        g_ueQueue       = nullptr; // captured from UE
static ID3D12DescriptorHeap*      g_srvHeap       = nullptr;
static ID3D12DescriptorHeap*      g_rtvHeap       = nullptr;
static ID3D12CommandAllocator*    g_alloc[NUM_BACK_BUFFERS] = {};
static ID3D12GraphicsCommandList* g_cmdList       = nullptr;
// Back buffers are NOT cached — fetched fresh every frame so swap chain
// resizes (device profile change, DLSS quality switch) never leave stale ptrs.
static D3D12_CPU_DESCRIPTOR_HANDLE g_rtvSlot      = {};  // single RTV slot, recreated each frame
static UINT                       g_rtvDescSize   = 0;
static ID3D12Fence*               g_fence         = nullptr;
static HANDLE                     g_fenceEvent    = nullptr;
static UINT64                     g_fenceSlot[NUM_BACK_BUFFERS] = {}; // last fence val per slot
static UINT64                     g_fenceVal      = 0;
static int                        g_bufCount      = 0;
static int                        g_srvAllocIdx   = 0;

// Hook trampolines
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present1)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSCFH)(IDXGIFactory2*, IUnknown*, HWND,
    const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,
    IDXGIOutput*, IDXGISwapChain1**);
typedef LRESULT(CALLBACK* PFN_WndProc)(HWND, UINT, WPARAM, LPARAM);

static PFN_Present      g_origPresent    = nullptr;
static PFN_Present1     g_origPresent1   = nullptr;
static PFN_CreateSCFH   g_origCreateSCFH = nullptr;
static PFN_WndProc      g_origWndProc    = nullptr;

// Per-frame render callback (set by the mod class)
static std::function<void()> g_renderFn;
static std::atomic<bool>     g_showOverlay{false};  // true when widget exists (starts D3D12 pipeline early)
static std::atomic<bool>     g_showContent{false};  // true when settings is actually open (flag file)

// ── WndProc hook (forwards input to ImGui) ────────────────────────────────────

// imgui_impl_win32.h intentionally hides this behind #if 0; copy the declaration as instructed.
IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static LRESULT CALLBACK WndProc_Hook(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp))
        return true;
    return g_origWndProc(hwnd, msg, wp, lp);
}

// ── ImGui SRV descriptor allocator (bump-style, 64 slots) ────────────────────

static void SrvAlloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu,
                     D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
{
    UINT sz = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    cpu->ptr = g_srvHeap->GetCPUDescriptorHandleForHeapStart().ptr + g_srvAllocIdx * sz;
    gpu->ptr = g_srvHeap->GetGPUDescriptorHandleForHeapStart().ptr + g_srvAllocIdx * sz;
    g_srvAllocIdx++;
}
static void SrvFree(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE,
                    D3D12_GPU_DESCRIPTOR_HANDLE) {}

// ── D3D12 + ImGui initialisation (called from first Present) ─────────────────

static bool InitD3DAndImGui(IDXGISwapChain* sc)
{
    // Get swap chain as IDXGISwapChain3 for GetCurrentBackBufferIndex
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3)))) return false;

    // Get D3D12 device
    if (FAILED(sc3->GetDevice(IID_PPV_ARGS(&g_device)))) { sc3->Release(); return false; }

    // Swap chain descriptor – need format and buffer count
    DXGI_SWAP_CHAIN_DESC scDesc{};
    sc3->GetDesc(&scDesc);
    g_bufCount = (int)scDesc.BufferCount;
    if (g_bufCount > NUM_BACK_BUFFERS) g_bufCount = NUM_BACK_BUFFERS;

    // RTV descriptor heap — 1 slot only; we recreate the RTV every frame from
    // the live back buffer pointer so resize never leaves us with stale resources.
    D3D12_DESCRIPTOR_HEAP_DESC rtvHD{};
    rtvHD.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHD.NumDescriptors = 1;
    rtvHD.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(g_device->CreateDescriptorHeap(&rtvHD, IID_PPV_ARGS(&g_rtvHeap))))
        return false;
    g_rtvDescSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    g_rtvSlot = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();

    // SRV descriptor heap (shader-visible, 64 slots for ImGui textures)
    D3D12_DESCRIPTOR_HEAP_DESC srvHD{};
    srvHD.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHD.NumDescriptors = 64;
    srvHD.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_device->CreateDescriptorHeap(&srvHD, IID_PPV_ARGS(&g_srvHeap))))
        return false;

    // Command allocators (one per back buffer)
    for (int i = 0; i < g_bufCount; ++i)
        g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc[i]));

    // Command list
    g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                g_alloc[0], nullptr, IID_PPV_ARGS(&g_cmdList));
    g_cmdList->Close();

    // Fence for serialising our draw calls across frames
    g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
    g_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    g_fenceVal   = 0;

    // g_ueQueue was captured in CreateSCFH_Hook from the pDevice argument
    // (ID3D12CommandQueue* in D3D12). Submitting on UE's own queue serialises
    // our overlay commands after UE's frame work.
    if (!g_ueQueue) return false;

    // Initialise ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(g_gameHwnd);

    ImGui_ImplDX12_InitInfo di{};
    di.Device              = g_device;
    di.CommandQueue        = g_ueQueue;
    di.NumFramesInFlight   = g_bufCount;
    di.RTVFormat           = scDesc.BufferDesc.Format;
    di.DSVFormat           = DXGI_FORMAT_UNKNOWN;
    di.SrvDescriptorHeap   = g_srvHeap;
    di.SrvDescriptorAllocFn = SrvAlloc;
    di.SrvDescriptorFreeFn  = SrvFree;
    if (!ImGui_ImplDX12_Init(&di)) return false;

    sc3->Release();

    // Hook game WndProc for mouse/keyboard input
    g_origWndProc = reinterpret_cast<PFN_WndProc>(
        SetWindowLongPtrW(g_gameHwnd, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(WndProc_Hook)));

    return true;
}

// ── Per-frame render (called from Present hooks) ──────────────────────────────

static void RenderOverlay(IDXGISwapChain3* sc3)
{
    if (!g_showOverlay.load(std::memory_order_relaxed)) return;

    // Fetch back buffer LIVE — never use a cached pointer.
    // Swap chain resizes (device profile change, DLSS quality switch) replace
    // the underlying ID3D12Resource objects; cached pointers become dangling.
    UINT fi = sc3->GetCurrentBackBufferIndex();
    if (fi >= (UINT)g_bufCount) fi = 0;

    ID3D12Resource* backBuf = nullptr;
    if (FAILED(sc3->GetBuffer(fi, IID_PPV_ARGS(&backBuf)))) return;

    // Recreate the RTV for this frame's back buffer in our single heap slot.
    g_device->CreateRenderTargetView(backBuf, nullptr, g_rtvSlot);

    // Wait for our previous submission on this slot — per-slot tracking so
    // we never reset an allocator the GPU is still reading.
    if (g_fence->GetCompletedValue() < g_fenceSlot[fi])
    {
        g_fence->SetEventOnCompletion(g_fenceSlot[fi], g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, 100);
    }

    g_alloc[fi]->Reset();
    g_cmdList->Reset(g_alloc[fi], nullptr);

    // Transition back-buffer PRESENT → RENDER_TARGET
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = backBuf;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_cmdList->ResourceBarrier(1, &barrier);

    g_cmdList->OMSetRenderTargets(1, &g_rtvSlot, FALSE, nullptr);
    g_cmdList->SetDescriptorHeaps(1, &g_srvHeap);

    // ImGui frame
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    {
        std::lock_guard<std::mutex> lk(g_overlayMtx);
        if (g_renderFn) g_renderFn();
    }

    ImGui::Render();
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_cmdList);

    // Transition RENDER_TARGET → PRESENT
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    g_cmdList->ResourceBarrier(1, &barrier);
    g_cmdList->Close();

    backBuf->Release();

    // Submit and CPU-wait so the GPU has finished (back buffer back in PRESENT
    // state) before Streamline/DLSS-G processes the Present call.
    ID3D12CommandList* lists[] = {g_cmdList};
    g_ueQueue->ExecuteCommandLists(1, lists);
    g_ueQueue->Signal(g_fence, ++g_fenceVal);
    g_fenceSlot[fi] = g_fenceVal;
    g_fence->SetEventOnCompletion(g_fenceVal, g_fenceEvent);
    WaitForSingleObject(g_fenceEvent, 100);
}

// ── Present hooks ─────────────────────────────────────────────────────────────

static bool TryInitOnFirstPresent(IDXGISwapChain* sc)
{
    if (g_d3dReady) return true;

    DXGI_SWAP_CHAIN_DESC desc{};
    sc->GetDesc(&desc);
    g_gameHwnd = desc.OutputWindow;
    if (!g_gameHwnd) return false;

    if (InitD3DAndImGui(sc))
        g_d3dReady = true;

    return g_d3dReady;
}

static HRESULT STDMETHODCALLTYPE Present_Hook(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    TryInitOnFirstPresent(sc);
    if (g_d3dReady)
    {
        IDXGISwapChain3* sc3 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc3))))
        {
            RenderOverlay(sc3);
            sc3->Release();
        }
    }
    return g_origPresent(sc, sync, flags);
}

static HRESULT STDMETHODCALLTYPE Present1_Hook(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                               const DXGI_PRESENT_PARAMETERS* params)
{
    TryInitOnFirstPresent(sc);
    if (g_d3dReady)
    {
        IDXGISwapChain3* sc3 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc3))))
        {
            RenderOverlay(sc3);
            sc3->Release();
        }
    }
    return g_origPresent1(sc, sync, flags, params);
}

// ── CreateSwapChainForHwnd hook – captures the swap chain & hooks Present ─────

static HRESULT STDMETHODCALLTYPE CreateSCFH_Hook(
    IDXGIFactory2* factory, IUnknown* pDevice, HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC1* pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFSDesc,
    IDXGIOutput* pOutput, IDXGISwapChain1** ppSC)
{
    HRESULT hr = g_origCreateSCFH(factory, pDevice, hWnd, pDesc, pFSDesc, pOutput, ppSC);

    if (SUCCEEDED(hr) && ppSC && *ppSC && !g_origPresent)
    {
        // In D3D12, pDevice passed to CreateSwapChainForHwnd is ID3D12CommandQueue*.
        // Capture it so we can submit our overlay on UE's own queue (proper serialisation).
        if (!g_ueQueue)
        {
            ID3D12CommandQueue* q = nullptr;
            if (SUCCEEDED(reinterpret_cast<IUnknown*>(pDevice)->QueryInterface(IID_PPV_ARGS(&q))))
                g_ueQueue = q; // AddRef'd; released in UninstallHooks
        }

        IDXGISwapChain1* sc1 = *ppSC;
        void** vtbl = *reinterpret_cast<void***>(sc1);

        // Present is vtable[8], Present1 is vtable[22] on IDXGISwapChain1
        MH_CreateHook(vtbl[8],  reinterpret_cast<void*>(Present_Hook),
                      reinterpret_cast<void**>(&g_origPresent));
        MH_CreateHook(vtbl[22], reinterpret_cast<void*>(Present1_Hook),
                      reinterpret_cast<void**>(&g_origPresent1));
        MH_EnableHook(vtbl[8]);
        MH_EnableHook(vtbl[22]);
    }

    return hr;
}

// ── Hook installation ─────────────────────────────────────────────────────────

static void InstallHooks()
{
    MH_Initialize();

    // Create a temporary DXGI factory (safe – no D3D12 resources) to get the
    // CreateSwapChainForHwnd vtable address.
    IDXGIFactory2* tmp = nullptr;
    if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&tmp))))
    {
        void** vtbl = *reinterpret_cast<void***>(tmp);
        // CreateSwapChainForHwnd is vtable[15] on IDXGIFactory2
        MH_CreateHook(vtbl[15], reinterpret_cast<void*>(CreateSCFH_Hook),
                      reinterpret_cast<void**>(&g_origCreateSCFH));
        MH_EnableHook(vtbl[15]);
        tmp->Release();
    }
}

static void UninstallHooks()
{
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();

    if (g_origWndProc && g_gameHwnd)
        SetWindowLongPtrW(g_gameHwnd, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(g_origWndProc));

    if (g_d3dReady)
    {
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }

    // Release D3D12 resources
    if (g_fence)     { g_fence->Release();     g_fence = nullptr; }
    if (g_fenceEvent){ CloseHandle(g_fenceEvent); g_fenceEvent = nullptr; }
    if (g_cmdList)   { g_cmdList->Release();   g_cmdList = nullptr; }
    for (auto& a : g_alloc) { if (a) { a->Release(); a = nullptr; } }
    if (g_rtvHeap)   { g_rtvHeap->Release();   g_rtvHeap = nullptr; }
    if (g_srvHeap)   { g_srvHeap->Release();   g_srvHeap = nullptr; }
    if (g_ueQueue)   { g_ueQueue->Release();   g_ueQueue = nullptr; }
    if (g_device)    { g_device->Release();     g_device = nullptr; }
}

// ── Mod class ─────────────────────────────────────────────────────────────────

class SN2ThirdPersonSettingsMod : public RC::CppUserModBase
{
    TPSettings m_saved;
    TPSettings m_edit;
    bool       m_dirty      = false;
    bool       m_showSaved  = false;
    float      m_flashTimer = 0.f;
    char       m_customKeyBuf[64]{};

    std::atomic<bool> m_unrealReady{false};
    int               m_pollTick = 0;

public:
    SN2ThirdPersonSettingsMod() : CppUserModBase()
    {
        ModName        = STR("SN2ThirdPersonSettings");
        ModVersion     = STR("3.0");
        ModDescription = STR("In-game settings overlay for SN2ThirdPersonMod");
        ModAuthors     = STR("rafa");

        m_saved = LoadSettings();
        m_edit  = m_saved;
        SyncCustomBuf();

        // Register our per-frame render callback for the D3D12 overlay
        {
            std::lock_guard<std::mutex> lk(g_overlayMtx);
            g_renderFn = [this] { RenderPanel(); };
        }

        // F9 = force-show toggle for testing
        register_keydown_event(Input::Key::F9, [this]() {
            g_showOverlay.store(!g_showOverlay.load(std::memory_order_relaxed),
                                std::memory_order_relaxed);
        });

        // Escape = immediate close of overlay (supplementary to IsActivated polling)
        register_keydown_event(Input::Key::ESCAPE, [this]() {
            g_showOverlay.store(false, std::memory_order_relaxed);
        });

        // Install DXGI/D3D12 hooks immediately – safe, only reads DXGI vtable
        InstallHooks();
    }

    ~SN2ThirdPersonSettingsMod() override
    {
        {
            std::lock_guard<std::mutex> lk(g_overlayMtx);
            g_renderFn = nullptr;
        }
        UninstallHooks();
    }

    auto on_unreal_init() -> void override
    {
        m_unrealReady = true;
    }

    auto on_update() -> void override
    {
        if (!m_unrealReady.load(std::memory_order_relaxed)) return;
        if (++m_pollTick < 6) return;
        m_pollTick = 0;

        // g_showOverlay: true as soon as the settings widget exists in GObjects.
        // Starting the D3D12 render pipeline BEFORE IsActivated fires avoids a
        // crash where Streamline/DLSS-G is mid-transition at that exact moment.
        auto* vm = RC::Unreal::UObjectGlobals::FindFirstOf(L"WBP_Settings2Screen_C");
        g_showOverlay.store(vm != nullptr, std::memory_order_relaxed);

        // g_showContent: true only when settings is actually open.
        // Lua polls IsActivated() every 300ms and writes "1"/"0" to flag file.
        // C++ GetFunctionByName is broken for Blueprint subclasses; Lua is the
        // only reliable way to call IsActivated().
        bool content = false;
        if (vm != nullptr)
        {
            std::ifstream f(GetModRootDir() / L"settings_open.flag");
            if (f.is_open()) { char c = '0'; f.get(c); content = (c == '1'); }
        }
        g_showContent.store(content, std::memory_order_relaxed);
    }

private:
    void RenderPanel()
    {
        if (!g_showContent.load(std::memory_order_relaxed)) return;

        const ImGuiIO& io = ImGui::GetIO();

        // Measured from mockup at 1920x1080; scales to any resolution.
        constexpr float kPanelLeft   = 25.f;    // px from left edge (gutter margin)
        constexpr float kTopFrac     = 0.1796f; // 194 / 1080
        constexpr float kBottomFrac  = 0.8000f; // (194 + 670) / 1080
        constexpr float kWidthFrac   = 0.1146f; // 220 / 1920

        const float panelX = kPanelLeft;
        const float panelY = io.DisplaySize.y * kTopFrac;
        const float panelW = io.DisplaySize.x * kWidthFrac;
        const float panelH = io.DisplaySize.y * (kBottomFrac - kTopFrac);

        ImGui::SetNextWindowPos(ImVec2(panelX, panelY), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(panelW, panelH), ImGuiCond_Always);

        constexpr ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoMove             |
            ImGuiWindowFlags_NoResize           |
            ImGuiWindowFlags_NoCollapse         |
            ImGuiWindowFlags_NoSavedSettings    |
            ImGuiWindowFlags_AlwaysVerticalScrollbar;

        if (!ImGui::Begin("Third Person Camera", nullptr, flags))
        {
            ImGui::End();
            return;
        }

        RenderContent();
        ImGui::End();
    }

    void RenderContent()
    {
        // ── BEHAVIOUR ─────────────────────────────────────────────────────────
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.80f, 1.00f, 1.00f));
        ImGui::TextUnformatted("BEHAVIOUR");
        ImGui::PopStyleColor();
        ImGui::Spacing();

        if (ImGui::Checkbox("Hide HUD in third-person", &m_edit.hideHUD))
            m_dirty = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Hides scuba mask, oxygen bar, and health bar\n"
                "while third-person mode is active.\n\n"
                "Takes effect on the next toggle.");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ── TOGGLE KEY ────────────────────────────────────────────────────────
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.80f, 1.00f, 1.00f));
        ImGui::TextUnformatted("TOGGLE KEY");
        ImGui::PopStyleColor();
        ImGui::Spacing();

        int         sel     = FindKeyIndex(m_edit.toggleKey);
        const char* preview = sel >= 0 ? k_keys[sel] : "Custom...";

        {
            const float helpW = ImGui::CalcTextSize(" (?)").x + ImGui::GetStyle().ItemSpacing.x;
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - helpW);
        }
        if (ImGui::BeginCombo("##key_combo", preview))
        {
            for (int i = 0; i < k_key_count; ++i)
            {
                bool is_sel = (i == sel);
                if (ImGui::Selectable(k_keys[i], is_sel))
                {
                    m_edit.toggleKey = k_keys[i];
                    SyncCustomBuf();
                    m_dirty = true;
                }
                if (is_sel) ImGui::SetItemDefaultFocus();
            }
            if (ImGui::Selectable("Custom...", sel < 0))
            {
                m_edit.toggleKey = m_customKeyBuf;
                m_dirty = true;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Key names match UE4SS input identifiers, e.g.:\n"
                "  F3, G, NUM_ONE, INSERT ...\n\n"
                "Key changes require Ctrl+R (hot-reload) to apply.");

        if (sel < 0)
        {
            ImGui::Spacing();
            ImGui::TextUnformatted("Custom key:");
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputText("##key_custom", m_customKeyBuf, sizeof(m_customKeyBuf)))
            {
                m_edit.toggleKey = m_customKeyBuf;
                m_dirty = true;
            }
        }

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.80f, 0.30f, 0.85f));
        ImGui::TextWrapped("Key changes require Ctrl+R to take effect.");
        ImGui::PopStyleColor();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ── Save / Reset ──────────────────────────────────────────────────────
        if (m_dirty)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.75f, 0.20f, 1.00f));
            ImGui::TextUnformatted("*  Unsaved changes");
            ImGui::PopStyleColor();
            ImGui::SameLine(0.f, 20.f);
        }

        if (m_dirty)
        {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.18f, 0.50f, 0.18f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.68f, 0.28f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.12f, 0.38f, 0.12f, 1.00f));
        }
        if (ImGui::Button("Save"))
        {
            m_saved      = m_edit;
            SaveSettings(m_saved);
            m_dirty      = false;
            m_showSaved  = true;
            m_flashTimer = 2.5f;
        }
        if (m_dirty) ImGui::PopStyleColor(3);

        ImGui::SameLine(0.f, 10.f);
        if (ImGui::Button("Reset to defaults"))
        {
            TPSettings def{};
            m_edit = def;
            SyncCustomBuf();
            m_dirty = (m_edit != m_saved);
        }

        if (m_showSaved)
        {
            ImGui::SameLine(0.f, 14.f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 1.00f, 0.35f, 1.00f));
            ImGui::TextUnformatted("Saved!");
            ImGui::PopStyleColor();
            m_flashTimer -= ImGui::GetIO().DeltaTime;
            if (m_flashTimer <= 0.f) m_showSaved = false;
        }
    }

    void SyncCustomBuf()
    {
        strncpy_s(m_customKeyBuf, m_edit.toggleKey.c_str(), sizeof(m_customKeyBuf) - 1);
    }
};

// ── Exports ───────────────────────────────────────────────────────────────────

extern "C"
{
    __declspec(dllexport) RC::CppUserModBase* start_mod()
    {
        return new SN2ThirdPersonSettingsMod();
    }

    __declspec(dllexport) void uninstall_mod(RC::CppUserModBase* m)
    {
        delete m;
    }
}
