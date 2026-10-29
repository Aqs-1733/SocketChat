// Win32 窗口截图工具。
// 捕获目标窗口画面。
#include <windows.h>
#include <cstdio>
#include <vector>

// 将窗口画面保存为 BMP 文件。
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::fwprintf(stderr, L"Usage: capture_window.exe <output.bmp>\n");
        return 1;
    }

    const HWND target = FindWindowW(
        L"SimpleChatWin32Window", L"SimpleChat | C++ 原生 TCP 聊天室");
    if (target == nullptr) {
        std::fwprintf(stderr, L"Target GUI window was not found.\n");
        return 2;
    }

    RECT bounds{};
    if (!GetWindowRect(target, &bounds)) {
        return 3;
    }
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0) {
        return 4;
    }

    const HDC window_dc = GetWindowDC(target);
    const HDC memory_dc = CreateCompatibleDC(window_dc);
    const HBITMAP bitmap = CreateCompatibleBitmap(window_dc, width, height);
    if (window_dc == nullptr || memory_dc == nullptr || bitmap == nullptr) {
        if (bitmap != nullptr) DeleteObject(bitmap);
        if (memory_dc != nullptr) DeleteDC(memory_dc);
        if (window_dc != nullptr) ReleaseDC(target, window_dc);
        return 5;
    }

    const HGDIOBJ old_bitmap = SelectObject(memory_dc, bitmap);
    BOOL captured = PrintWindow(target, memory_dc, PW_RENDERFULLCONTENT);
    if (!captured) {
        captured = BitBlt(memory_dc, 0, 0, width, height, window_dc, 0, 0, SRCCOPY);
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    const DWORD pixels_size = static_cast<DWORD>(width * height * 4);
    std::vector<BYTE> pixels(pixels_size);
    const int rows = GetDIBits(memory_dc, bitmap, 0, static_cast<UINT>(height),
                               pixels.data(), &info, DIB_RGB_COLORS);

    const HANDLE file = CreateFileW(argv[1], GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    BITMAPFILEHEADER header{};
    header.bfType = 0x4D42;
    header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    header.bfSize = header.bfOffBits + pixels_size;

    const BOOL saved = captured && rows == height && file != INVALID_HANDLE_VALUE &&
        WriteFile(file, &header, sizeof(header), &written, nullptr) &&
        written == sizeof(header) &&
        WriteFile(file, &info.bmiHeader, sizeof(info.bmiHeader), &written, nullptr) &&
        written == sizeof(info.bmiHeader) &&
        WriteFile(file, pixels.data(), pixels_size, &written, nullptr) &&
        written == pixels_size;
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    SelectObject(memory_dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(memory_dc);
    ReleaseDC(target, window_dc);
    return saved ? 0 : 6;
}
