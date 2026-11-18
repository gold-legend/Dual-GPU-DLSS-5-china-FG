// Standalone DLSSNR.ControlMask A/B test. No game or game file is touched.
#include <windows.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cwchar>
#include <string>

int wmain(int argc, wchar_t **argv)
{
    int neural = -1, other = -1;
    std::wstring snippet, log = L"nrmask-results.txt";
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (!wcscmp(argv[i], L"--neural")) neural = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--other")) other = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--snippet")) snippet = argv[++i];
        else if (!wcscmp(argv[i], L"--log")) log = argv[++i];
    }
    IDXGIFactory1 *factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 3;
    IDXGIAdapter1 *adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; ++i)
    {
        DXGI_ADAPTER_DESC1 d{};
        adapter->GetDesc1(&d);
        if (d.VendorId == 0x10DE && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
        {
            wprintf(L"NVIDIA adapter[%u]: %s\n", i, d.Description);
            if (neural < 0) neural = int(i);
            else if (other < 0 && int(i) != neural) other = int(i);
        }
        adapter->Release();
    }
    factory->Release();
    if (neural < 0 || snippet.empty())
    {
        printf("usage: nrmaskcheck.exe --snippet <nvngx_dlssnr.dll> [--neural N] [--other N] [--log file]\n");
        return 3;
    }
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dll = exe;
    dll.erase(dll.find_last_of(L'\\') + 1);
    dll += L"nvngx.dll_nrmaskcheck.dll";
    HMODULE module = LoadLibraryW(dll.c_str());
    if (!module) { wprintf(L"Cannot load %s (error %lu)\n", dll.c_str(), GetLastError()); return 3; }
    using run_fn = int (*)(int, int, const wchar_t *, const wchar_t *);
    auto run = reinterpret_cast<run_fn>(GetProcAddress(module, "nrmask_run"));
    if (!run) return 3;
    wprintf(L"Testing neural adapter[%d], other adapter[%d], snippet %s\n", neural, other, snippet.c_str());
    return run(neural, other, snippet.c_str(), log.c_str());
}
