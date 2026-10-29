#include <windows.h>
#include <algorithm>
#include <cwctype>
#include <cstdio>
#include <string>

// 将窗口标题转换为小写。
std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

// 保存窗口查找结果。
struct WindowSearch {
    std::wstring keyword;
    HWND handle = nullptr;
    std::wstring title;
};

// 启用高 DPI 截图坐标。
void EnableDpiAwareness() {
    using SetDpiAwarenessContextProc = BOOL(WINAPI*)(HANDLE);
    const auto set_context = reinterpret_cast<SetDpiAwarenessContextProc>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (set_context != nullptr &&
        set_context(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)))) {
        return;
    }

    using SetDpiAwareProc = BOOL(WINAPI*)();
    const auto set_aware = reinterpret_cast<SetDpiAwareProc>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDPIAware"));
    if (set_aware != nullptr) {
        set_aware();
    }
}

// 查找标题匹配的顶层窗口。
BOOL CALLBACK FindTargetWindow(HWND handle, LPARAM rawSearch) {
    auto* search = reinterpret_cast<WindowSearch*>(rawSearch);
    if (!IsWindowVisible(handle) || IsIconic(handle)) {
        return TRUE;
    }

    RECT rect{};
    if (!GetWindowRect(handle, &rect) || rect.right - rect.left < 400 ||
        rect.bottom - rect.top < 300) {
        return TRUE;
    }

    wchar_t titleBuffer[1024]{};
    GetWindowTextW(handle, titleBuffer, static_cast<int>(std::size(titleBuffer)));
    const std::wstring title(titleBuffer);
    if (!title.empty() && ToLower(title).find(search->keyword) != std::wstring::npos) {
        search->handle = handle;
        search->title = title;
        return FALSE;
    }
    return TRUE;
}

// 将窗口画面保存为 BMP 文件。
bool CaptureWindowToBmp(HWND target, const wchar_t* outputPath) {
    RECT rect{};
    if (!GetWindowRect(target, &rect)) {
        return false;
    }

    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) {
        return false;
    }

    HDC screenDc = GetDC(nullptr);
    HDC memoryDc = CreateCompatibleDC(screenDc);
    HBITMAP bitmap = CreateCompatibleBitmap(screenDc, width, height);
    HGDIOBJ previous = SelectObject(memoryDc, bitmap);

    // 捕获窗口画面。
    if (!PrintWindow(target, memoryDc, PW_RENDERFULLCONTENT)) {
        BitBlt(memoryDc, 0, 0, width, height, screenDc, rect.left, rect.top, SRCCOPY);
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    const DWORD byteCount = static_cast<DWORD>(width) * static_cast<DWORD>(height) * 4U;

    auto* pixels = new BYTE[byteCount];
    const int lines = GetDIBits(memoryDc, bitmap, 0, static_cast<UINT>(height), pixels, &info, DIB_RGB_COLORS);

    bool saved = false;
    if (lines == height) {
        BITMAPFILEHEADER header{};
        header.bfType = 0x4D42;
        header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        header.bfSize = header.bfOffBits + byteCount;

        HANDLE file = CreateFileW(outputPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            const bool headerOk = WriteFile(file, &header, sizeof(header), &written, nullptr) && written == sizeof(header);
            const bool infoOk = WriteFile(file, &info.bmiHeader, sizeof(info.bmiHeader), &written, nullptr) && written == sizeof(info.bmiHeader);
            const bool pixelsOk = WriteFile(file, pixels, byteCount, &written, nullptr) && written == byteCount;
            saved = headerOk && infoOk && pixelsOk;
            CloseHandle(file);
        }
    }

    delete[] pixels;
    SelectObject(memoryDc, previous);
    DeleteObject(bitmap);
    DeleteDC(memoryDc);
    ReleaseDC(nullptr, screenDc);
    return saved;
}

int wmain(int argc, wchar_t* argv[]) {
    EnableDpiAwareness();
    SetConsoleOutputCP(CP_UTF8);

    if (argc != 3) {
        std::fwprintf(stderr, L"用法: capture_named_window.exe <窗口标题关键字> <输出BMP路径>\n");
        return 2;
    }

    WindowSearch search{ToLower(argv[1])};
    EnumWindows(FindTargetWindow, reinterpret_cast<LPARAM>(&search));
    if (search.handle == nullptr) {
        std::fwprintf(stderr, L"未找到可见窗口: %ls\n", argv[1]);
        return 3;
    }

    if (!CaptureWindowToBmp(search.handle, argv[2])) {
        std::fwprintf(stderr, L"窗口截图保存失败: %ls\n", argv[2]);
        return 4;
    }

    std::fwprintf(stdout, L"CAPTURED=1\nOUTPUT=%ls\n", argv[2]);
    return 0;
}


