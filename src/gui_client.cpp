// 原生 Win32 C++ 聊天客户端。
// 网络层调用 Winsock2 函数。
// 界面层调用 Win32 API。

#ifdef _WIN32

#include "protocol.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr UINT WM_CHAT_FRAME = WM_APP + 11;
constexpr UINT WM_CHAT_DISCONNECTED = WM_APP + 12;

constexpr int ID_CONNECT = 1001;
constexpr int ID_SEND = 1002;
constexpr int ID_WHO = 1003;
constexpr int ID_QUIT = 1004;

HWND main_window = nullptr;
HWND title_label = nullptr;
HWND tip_label = nullptr;
HWND ip_label = nullptr;
HWND port_label = nullptr;
HWND name_label = nullptr;
HWND ip_input = nullptr;
HWND port_input = nullptr;
HWND name_input = nullptr;
HWND chat_log = nullptr;
HWND message_input = nullptr;
HWND status_label = nullptr;
HWND connect_button = nullptr;
HWND send_button = nullptr;
HWND users_button = nullptr;
HWND quit_button = nullptr;

HFONT main_font = nullptr;
HFONT title_font = nullptr;
HBRUSH white_brush = nullptr;

SOCKET server_socket = INVALID_SOCKET;
HANDLE receive_thread = nullptr;
std::atomic<bool> connected{false};
bool joined = false;
std::uint64_t sequence = 1;

// 读取编辑框文本。
std::wstring get_control_text(HWND control) {
    const int length = GetWindowTextLengthW(control);
    if (length <= 0) {
        return {};
    }

    std::wstring text(static_cast<std::size_t>(length + 1), L'\0');
    const int copied = GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(copied));
    return text;
}

// 将 UTF-16 文本转换为 UTF-8。
std::string to_utf8(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }

    const int count = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS,
        wide.data(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr);

    if (count <= 0) {
        return {};
    }

    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS,
        wide.data(), static_cast<int>(wide.size()),
        result.data(), count, nullptr, nullptr);
    return result;
}

// 将协议消息转换为 UTF-16。
std::wstring from_utf8(const std::string& text) {
    if (text.empty()) {
        return {};
    }

    const int count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS,
        text.data(), static_cast<int>(text.size()), nullptr, 0);

    if (count <= 0) {
        return L"[UTF-8 解码失败]";
    }

    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS,
        text.data(), static_cast<int>(text.size()), result.data(), count);
    return result;
}

// 向聊天区域追加一行文本。
void append_chat(const std::wstring& message) {
    const int end = GetWindowTextLengthW(chat_log);
    SendMessageW(chat_log, EM_SETSEL, end, end);

    const std::wstring line = message + L"\r\n";
    SendMessageW(chat_log, EM_REPLACESEL, FALSE,
                 reinterpret_cast<LPARAM>(line.c_str()));
    SendMessageW(chat_log, EM_SCROLLCARET, 0, 0);
}

// 更新控件状态。
void update_controls() {
    const bool online = connected.load();

    EnableWindow(connect_button, !online);
    EnableWindow(ip_input, !online);
    EnableWindow(port_input, !online);
    EnableWindow(name_input, !online);
    EnableWindow(send_button, online && joined);
    EnableWindow(users_button, online && joined);
    EnableWindow(quit_button, online);

    const wchar_t* status = L"状态：未连接";
    if (online) {
        status = joined ? L"状态：已连接，可以聊天" : L"状态：等待服务器确认登录";
    }
    SetWindowTextW(status_label, status);
}

// 按窗口大小安排控件。
void layout_controls(int width, int height) {
    constexpr int margin = 24;
    constexpr int top_height = 34;

    MoveWindow(title_label, margin, 16, width - 48, 34, TRUE);
    MoveWindow(tip_label, margin, 51, width - 48, 24, TRUE);

    MoveWindow(ip_label, margin, 90, 60, 24, TRUE);
    MoveWindow(ip_input, 88, 86, 135, top_height, TRUE);
    MoveWindow(port_label, 233, 90, 44, 24, TRUE);
    MoveWindow(port_input, 275, 86, 76, top_height, TRUE);
    MoveWindow(name_label, 362, 90, 60, 24, TRUE);
    MoveWindow(name_input, 423, 86, 120, top_height, TRUE);
    MoveWindow(connect_button, width - 160, 86, 135, top_height, TRUE);
    MoveWindow(status_label, margin, 131, width - 48, 26, TRUE);

    const int message_y = height - 94;
    MoveWindow(chat_log, margin, 164, width - 48, message_y - 180, TRUE);
    MoveWindow(message_input, margin, message_y, width - 346, 38, TRUE);
    MoveWindow(send_button, width - 305, message_y, 86, 38, TRUE);
    MoveWindow(users_button, width - 211, message_y, 86, 38, TRUE);
    MoveWindow(quit_button, width - 117, message_y, 92, 38, TRUE);
}

// 接收线程提取 TCP 完整帧。
// 通过 PostMessageW 通知主线程。
DWORD WINAPI receive_worker(LPVOID parameter) {
    const SOCKET connection = static_cast<SOCKET>(
        reinterpret_cast<UINT_PTR>(parameter));

    std::string pending;
    char buffer[4096];

    for (;;) {
        const int received = ::recv(connection, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            break;
        }

        std::vector<std::string> frames;
        if (!chat::extract_frames(
                pending, buffer, static_cast<std::size_t>(received), frames)) {
            break;
        }

        for (const std::string& frame : frames) {
            auto* copy = new std::string(frame);
            const BOOL posted = PostMessageW(
                main_window, WM_CHAT_FRAME, 0, reinterpret_cast<LPARAM>(copy));
            if (!posted) {
                delete copy;
            }
        }
    }

    PostMessageW(main_window, WM_CHAT_DISCONNECTED, 0, 0);
    return 0;
}

// 解析服务器协议消息。
void display_protocol_frame(const std::string& frame) {
    if (frame.rfind("WELCOME|", 0) == 0) {
        joined = true;
        update_controls();
        append_chat(L"[登录成功] " + from_utf8(frame.substr(8)));
        return;
    }

    if (frame.rfind("MSG|", 0) == 0) {
        const auto fields = chat::split_fields(frame, 4);
        if (fields.size() == 5) {
            append_chat(L"[" + from_utf8(fields[1]) + L"] " +
                        from_utf8(fields[2]) + L"：" + from_utf8(fields[4]));
        }
        return;
    }

    if (frame.rfind("NOTICE|", 0) == 0) {
        const auto fields = chat::split_fields(frame, 2);
        if (fields.size() == 3) {
            append_chat(L"[系统通知] " + from_utf8(fields[2]) +
                        L"　[" + from_utf8(fields[1]) + L"]");
        }
        return;
    }

    if (frame.rfind("USERS|", 0) == 0) {
        append_chat(L"[当前在线] " + from_utf8(frame.substr(6)));
        return;
    }

    if (frame.rfind("ERR|", 0) == 0) {
        append_chat(L"[协议错误] " + from_utf8(frame.substr(4)));
        return;
    }

    if (frame.rfind("BYE|", 0) == 0) {
        append_chat(L"[退出确认] " + from_utf8(frame.substr(4)));
        return;
    }

    append_chat(L"[收到数据] " + from_utf8(frame));
}

// 解析端口数值。
bool parse_port_number(const std::string& text, unsigned short& port) {
    if (text.empty()) {
        return false;
    }

    unsigned long value = 0;
    for (char character : text) {
        if (character < '0' || character > '9') {
            return false;
        }
        value = value * 10 + static_cast<unsigned>(character - '0');
        if (value > 65535) {
            return false;
        }
    }

    if (value == 0) {
        return false;
    }
    port = static_cast<unsigned short>(value);
    return true;
}

// 建立连接并启动接收线程。
void connect_to_server() {
    const std::wstring wide_ip = get_control_text(ip_input);
    const std::wstring wide_port = get_control_text(port_input);
    const std::string ip = to_utf8(wide_ip);
    const std::string port_text = to_utf8(wide_port);
    const std::string user = to_utf8(get_control_text(name_input));

    if (!chat::is_valid_name(user)) {
        append_chat(L"[错误] 用户名只能包含1–16位英文字母、数字或下划线。");
        return;
    }

    unsigned short port = 0;
    if (!parse_port_number(port_text, port)) {
        append_chat(L"[错误] 端口必须是1–65535之间的整数。");
        return;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);

    if (inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1) {
        append_chat(L"[错误] 当前版本只支持 IPv4 地址。");
        return;
    }

    SOCKET connection = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (connection == INVALID_SOCKET) {
        append_chat(L"[错误] socket() 创建失败。");
        return;
    }

    if (::connect(connection,
                  reinterpret_cast<sockaddr*>(&address),
                  sizeof(address)) == SOCKET_ERROR) {
        append_chat(L"[错误] connect() 失败，请检查服务器与端口。");
        closesocket(connection);
        return;
    }

    if (!chat::send_frame(connection, "JOIN|" + user)) {
        append_chat(L"[错误] 发送 JOIN 失败。");
        closesocket(connection);
        return;
    }

    server_socket = connection;
    sequence = 1;
    joined = false;
    connected.store(true);
    update_controls();

    receive_thread = CreateThread(
        nullptr, 0, receive_worker,
        reinterpret_cast<LPVOID>(static_cast<UINT_PTR>(connection)),
        0, nullptr);

    if (receive_thread == nullptr) {
        append_chat(L"[错误] CreateThread() 失败。");
        shutdown(connection, SD_BOTH);
        closesocket(connection);
        server_socket = INVALID_SOCKET;
        connected.store(false);
        update_controls();
        return;
    }

    append_chat(L"[TCP 已连接] " + wide_ip + L":" + wide_port);
}

// 发送 TEXT 协议消息。
void send_message() {
    if (!connected.load() || !joined) {
        return;
    }

    const std::string body = to_utf8(get_control_text(message_input));
    if (!chat::is_valid_body(body)) {
        append_chat(L"[错误] 消息应为1–4096字节的有效UTF-8单行文本。");
        return;
    }

    const std::string frame = "TEXT|" + std::to_string(sequence) + "|" + body;
    if (!chat::send_frame(server_socket, frame)) {
        append_chat(L"[错误] send() 发送失败。");
        return;
    }

    ++sequence;
    SetWindowTextW(message_input, L"");
    SetFocus(message_input);
}

// 发送 QUIT 并关闭写方向。
void quit_chat() {
    if (!connected.load()) {
        return;
    }

    chat::send_frame(server_socket, "QUIT");
    shutdown(server_socket, SD_SEND);
    EnableWindow(quit_button, FALSE);
}

// 创建 Win32 控件。
HWND make_control(const wchar_t* class_name,
                  const wchar_t* initial_text,
                  DWORD style,
                  DWORD extended_style,
                  int identifier,
                  HWND parent) {
    HWND control = CreateWindowExW(
        extended_style, class_name, initial_text,
        style | WS_CHILD | WS_VISIBLE,
        0, 0, 80, 28, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(identifier)),
        GetModuleHandleW(nullptr), nullptr);

    SendMessageW(control, WM_SETFONT,
                 reinterpret_cast<WPARAM>(main_font), TRUE);
    return control;
}

// 处理窗口消息和资源回收。
LRESULT CALLBACK window_procedure(HWND window, UINT message,
                                  WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        main_window = window;
        main_font = CreateFontW(
            -19, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
        title_font = CreateFontW(
            -27, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
        white_brush = CreateSolidBrush(RGB(255, 255, 255));

        title_label = make_control(L"STATIC", L"TCP 多人聊天室", SS_LEFT, 0, 0, window);
        SendMessageW(title_label, WM_SETFONT,
                     reinterpret_cast<WPARAM>(title_font), TRUE);
        tip_label = make_control(
            L"STATIC", L"计算机网络实验一   |   C++17 · Winsock2 · SimpleChat/1.0",
            SS_LEFT, 0, 0, window);

        ip_label = make_control(L"STATIC", L"服务器", SS_LEFT, 0, 0, window);
        ip_input = make_control(
            L"EDIT", L"127.0.0.1", WS_BORDER | ES_AUTOHSCROLL,
            WS_EX_CLIENTEDGE, 0, window);
        port_label = make_control(L"STATIC", L"端口", SS_LEFT, 0, 0, window);
        port_input = make_control(
            L"EDIT", L"8888", WS_BORDER | ES_AUTOHSCROLL,
            WS_EX_CLIENTEDGE, 0, window);
        name_label = make_control(L"STATIC", L"用户名", SS_LEFT, 0, 0, window);
        name_input = make_control(
            L"EDIT", L"Alice", WS_BORDER | ES_AUTOHSCROLL,
            WS_EX_CLIENTEDGE, 0, window);
        connect_button = make_control(
            L"BUTTON", L"连接服务器", WS_TABSTOP | BS_PUSHBUTTON,
            0, ID_CONNECT, window);
        status_label = make_control(
            L"STATIC", L"状态：未连接", SS_LEFT, 0, 0, window);

        chat_log = make_control(
            L"EDIT",
            L"SimpleChat/1.0 已准备就绪。\r\n"
            L"请输入服务器地址、端口和用户名，然后点击连接。\r\n",
            WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL |
                ES_READONLY | WS_VSCROLL,
            WS_EX_CLIENTEDGE, 0, window);
        message_input = make_control(
            L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
            WS_EX_CLIENTEDGE, 0, window);
        send_button = make_control(
            L"BUTTON", L"发送", WS_TABSTOP | BS_DEFPUSHBUTTON,
            0, ID_SEND, window);
        users_button = make_control(
            L"BUTTON", L"在线用户", WS_TABSTOP | BS_PUSHBUTTON,
            0, ID_WHO, window);
        quit_button = make_control(
            L"BUTTON", L"退出", WS_TABSTOP | BS_PUSHBUTTON,
            0, ID_QUIT, window);

        update_controls();
        return 0;
    }
    case WM_SIZE: {
        const int width = LOWORD(lparam);
        const int height = HIWORD(lparam);
        if (chat_log != nullptr && width >= 700 && height >= 440) {
            layout_controls(width, height);
        }
        return 0;
    }
    case WM_COMMAND:
        if (HIWORD(wparam) == BN_CLICKED) {
            switch (LOWORD(wparam)) {
            case ID_CONNECT:
                connect_to_server();
                return 0;
            case ID_SEND:
                send_message();
                return 0;
            case ID_WHO:
                if (connected.load() && joined) {
                    chat::send_frame(server_socket, "WHO");
                }
                return 0;
            case ID_QUIT:
                quit_chat();
                return 0;
            }
        }
        break;
    case WM_CHAT_FRAME: {
        std::unique_ptr<std::string> frame(
            reinterpret_cast<std::string*>(lparam));
        display_protocol_frame(*frame);
        return 0;
    }
    case WM_CHAT_DISCONNECTED:
        if (receive_thread != nullptr) {
            WaitForSingleObject(receive_thread, INFINITE);
            CloseHandle(receive_thread);
            receive_thread = nullptr;
        }
        if (server_socket != INVALID_SOCKET) {
            closesocket(server_socket);
            server_socket = INVALID_SOCKET;
        }
        connected.store(false);
        joined = false;
        update_controls();
        append_chat(L"[连接结束] 可以重新输入用户名并登录。");
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
        SetBkColor(reinterpret_cast<HDC>(wparam), RGB(255, 255, 255));
        SetTextColor(reinterpret_cast<HDC>(wparam), RGB(25, 25, 25));
        return reinterpret_cast<INT_PTR>(white_brush);
    case WM_DESTROY:
        if (server_socket != INVALID_SOCKET) {
            shutdown(server_socket, SD_BOTH);
        }
        if (receive_thread != nullptr) {
            WaitForSingleObject(receive_thread, INFINITE);
            CloseHandle(receive_thread);
        }
        if (server_socket != INVALID_SOCKET) {
            closesocket(server_socket);
        }
        if (main_font != nullptr) {
            DeleteObject(main_font);
        }
        if (title_font != nullptr) {
            DeleteObject(title_font);
        }
        if (white_brush != nullptr) {
            DeleteObject(white_brush);
        }
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(window, message, wparam, lparam);
}

}

// Win32 程序入口。
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    if (!init_sockets()) {
        MessageBoxW(nullptr, L"WSAStartup() 失败。", L"SimpleChat", MB_ICONERROR);
        return 2;
    }

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.hInstance = instance;
    window_class.lpfnWndProc = window_procedure;
    window_class.lpszClassName = L"SimpleChatWin32Window";
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));

    if (!RegisterClassExW(&window_class)) {
        cleanup_sockets();
        return 2;
    }

    HWND window = CreateWindowExW(
        0, window_class.lpszClassName,
        L"SimpleChat | C++ 原生 TCP 聊天室",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 960, 680,
        nullptr, nullptr, instance, nullptr);

    if (window == nullptr) {
        cleanup_sockets();
        return 2;
    }

    ShowWindow(window, show_command);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        // 处理消息输入框的回车键。
        if (message.hwnd == message_input &&
            message.message == WM_KEYDOWN &&
            message.wParam == VK_RETURN) {
            send_message();
            continue;
        }

        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    cleanup_sockets();
    return static_cast<int>(message.wParam);
}

#else

// 提供平台兼容入口。
int main() {
    return 1;
}

#endif
