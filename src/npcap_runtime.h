#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>

namespace netlab {

// Both executables delay-load wpcap.dll. Select the installed Npcap directory
// before the first pcap call, including when launched directly from Explorer.
inline bool prepare_npcap(std::string& error) {
    wchar_t system_directory[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(system_directory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        error = "GetSystemDirectoryW failed";
        return false;
    }
    const std::wstring directory = std::wstring(system_directory) + L"\\Npcap";
    if (!SetDllDirectoryW(directory.c_str())) {
        error = "SetDllDirectoryW failed";
        return false;
    }
    // Keep this module loaded until process exit; the delay import uses it.
    if (LoadLibraryExW((directory + L"\\wpcap.dll").c_str(), nullptr,
                       LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                       LOAD_LIBRARY_SEARCH_SYSTEM32) == nullptr) {
        error = "Npcap runtime not found in the Windows system Npcap directory";
        return false;
    }
    error.clear();
    return true;
}

}
