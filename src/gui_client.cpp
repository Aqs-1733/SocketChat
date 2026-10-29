// 原生 Win32 C++ 聊天客户端。
// 界面使用 Win32 控件和 GDI 绘制。

#ifdef _WIN32

#include "protocol.h"
#include <commctrl.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iterator>
#include <filesystem>
#include <fstream>
#include <set>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr UINT WM_CHAT_FRAME = WM_APP + 11;
constexpr UINT WM_CHAT_DISCONNECTED = WM_APP + 12;
constexpr UINT BASE_DPI = 96;

constexpr int ID_CONNECT = 1001;
constexpr int ID_SEND = 1002;
constexpr int ID_REFRESH = 1003;
constexpr int ID_QUIT = 1004;
constexpr int ID_NAV_CHAT = 1005;
constexpr int ID_NAV_GROUP = 1006;
constexpr int ID_CREATE_GROUP = 1007;
constexpr int ID_ADD_GROUP_MEMBER = 1008;
constexpr int ID_LEAVE_GROUP = 1009;
constexpr int ID_CONVERSATIONS = 1010;
constexpr int ID_GROUP_LIST = 1011;
constexpr int ID_GROUP_NAME = 1012;
constexpr int ID_ONLINE_USERS = 1013;
constexpr UINT_PTR ID_DEMO_TIMER = 2001;
constexpr UINT_PTR ID_DISCONNECT_TIMER = 2002;

constexpr COLORREF COLOR_CANVAS = RGB(246, 248, 251);
constexpr COLORREF COLOR_CARD = RGB(255, 255, 255);
constexpr COLORREF COLOR_ACTIVITY = RGB(248, 250, 252);
constexpr COLORREF COLOR_HEADER = RGB(15, 23, 42);
constexpr COLORREF COLOR_HEADER_MUTED = RGB(203, 213, 225);
constexpr COLORREF COLOR_TEXT = RGB(30, 41, 59);
constexpr COLORREF COLOR_MUTED = RGB(100, 116, 139);
constexpr COLORREF COLOR_PRIMARY = RGB(13, 148, 136);
constexpr COLORREF COLOR_PRIMARY_DARK = RGB(15, 118, 110);
constexpr COLORREF COLOR_PRIMARY_LIGHT = RGB(204, 251, 241);
constexpr COLORREF COLOR_BORDER = RGB(226, 232, 240);
constexpr COLORREF COLOR_DANGER = RGB(220, 38, 38);

enum class ViewPage {
    chats,
    groups,
};

enum class ConversationKind {
    public_room,
    direct_chat,
    group_chat,
};

struct ConversationEntry {
    ConversationKind kind;
    std::wstring key;
    std::wstring title;
};

HWND main_window = nullptr;
HWND brand_label = nullptr;
HWND server_label = nullptr;
HWND port_label = nullptr;
HWND name_label = nullptr;
HWND ip_input = nullptr;
HWND port_input = nullptr;
HWND name_input = nullptr;
HWND connect_button = nullptr;
HWND refresh_button = nullptr;
HWND quit_button = nullptr;
HWND status_label = nullptr;
HWND nav_chat_button = nullptr;
HWND nav_group_button = nullptr;
HWND sidebar_title = nullptr;
HWND online_title = nullptr;
HWND conversation_list = nullptr;
HWND group_list = nullptr;
HWND online_user_list = nullptr;
HWND conversation_title = nullptr;
HWND activity_title = nullptr;
HWND group_name_input = nullptr;
HWND create_group_button = nullptr;
HWND join_group_button = nullptr;
HWND leave_group_button = nullptr;
HWND chat_log = nullptr;
HWND activity_log = nullptr;
HWND message_input = nullptr;
HWND send_button = nullptr;

HFONT regular_font = nullptr;
HFONT medium_font = nullptr;
HFONT title_font = nullptr;
HFONT small_font = nullptr;
HBRUSH canvas_brush = nullptr;
HBRUSH card_brush = nullptr;
HBRUSH header_brush = nullptr;
HBRUSH activity_brush = nullptr;

SOCKET server_socket = INVALID_SOCKET;
HANDLE receive_thread = nullptr;
std::atomic<bool> connected{false};
bool joined = false;
bool disconnecting = false;
bool close_pending = false;
std::uint64_t messages_sent = 0;
std::uint64_t messages_received = 0;
std::uint64_t messages_confirmed = 0;
std::set<std::uint64_t> awaiting_echo;
std::filesystem::path event_log_path;
std::uint64_t sequence = 1;
std::string current_user;
std::wstring server_address;
ViewPage active_view = ViewPage::chats;
ConversationKind active_kind = ConversationKind::public_room;
std::wstring active_key = L"public";
std::map<std::wstring, std::vector<std::wstring>> histories;
std::vector<std::wstring> activity_lines;
std::vector<std::string> online_users;
std::vector<std::string> group_names;
std::vector<std::string> direct_contacts;
std::vector<std::string> online_user_entries;
std::vector<ConversationEntry> conversation_entries;
bool demo_enabled = false;
std::wstring demo_role;
std::wstring requested_name;
int demo_step = 0;
UINT current_dpi = BASE_DPI;

void layout_controls(int width, int height);
void apply_fonts();
void update_controls();

// 将逻辑尺寸换算为当前显示器的像素尺寸。
int scale(int logical_size) {
    return MulDiv(logical_size, static_cast<int>(current_dpi), BASE_DPI);
}

// 读取窗口所在显示器的 DPI。
UINT get_window_dpi(HWND window) {
    using GetDpiForWindowProc = UINT(WINAPI*)(HWND);
    const auto get_dpi = reinterpret_cast<GetDpiForWindowProc>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    return get_dpi == nullptr ? BASE_DPI : get_dpi(window);
}

// 读取启动显示器的 DPI。
UINT get_system_dpi() {
    using GetDpiForSystemProc = UINT(WINAPI*)();
    const auto get_dpi = reinterpret_cast<GetDpiForSystemProc>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem"));
    if (get_dpi != nullptr) {
        return get_dpi();
    }

    HDC screen = GetDC(nullptr);
    const int dpi = screen == nullptr ? BASE_DPI : GetDeviceCaps(screen, LOGPIXELSX);
    if (screen != nullptr) {
        ReleaseDC(nullptr, screen);
    }
    return dpi > 0 ? static_cast<UINT>(dpi) : BASE_DPI;
}

// 在创建窗口前启用高 DPI 绘制。
void enable_dpi_awareness() {
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
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) {
        return {};
    }

    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
        static_cast<int>(wide.size()), result.data(), count, nullptr, nullptr);
    return result;
}

// 将 UTF-8 文本转换为 UTF-16。
std::wstring from_utf8(const std::string& text) {
    if (text.empty()) {
        return {};
    }

    const int count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) {
        return L"[UTF-8 解码失败]";
    }

    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), count);
    return result;
}

// 拆分逗号列表。
std::vector<std::string> split_csv(const std::string& text) {
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t comma = text.find(',', start);
        const std::size_t end = comma == std::string::npos ? text.size() : comma;
        if (end > start) {
            result.emplace_back(text.substr(start, end - start));
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return result;
}

// 生成私聊记录键。
std::wstring direct_key(const std::wstring& user) {
    return L"direct:" + user;
}

// 生成群聊记录键。
std::wstring group_key(const std::wstring& group) {
    return L"group:" + group;
}

// 滚动到末尾。
void scroll_log_to_bottom(HWND log) {
    if (log == nullptr) {
        return;
    }

    const int end = GetWindowTextLengthW(log);
    SendMessageW(log, EM_SETSEL, end, end);
    SendMessageW(log, EM_SCROLLCARET, 0, 0);
}

// 刷新文本记录。
void render_log_lines(HWND log, const std::vector<std::wstring>& lines) {
    if (log == nullptr) {
        return;
    }

    std::wstring text;
    for (const std::wstring& line : lines) {
        text.append(line);
        text.append(L"\r\n");
    }

    SetWindowTextW(log, text.c_str());
    scroll_log_to_bottom(log);
}

// 刷新聊天记录。
void render_active_history() {
    const auto iterator = histories.find(active_key);
    if (iterator == histories.end()) {
        render_log_lines(chat_log, {});
        return;
    }
    render_log_lines(chat_log, iterator->second);
}

// 刷新动态。
void render_activity_log() {
    render_log_lines(activity_log, activity_lines);
}

// 追加聊天记录。
void append_history(const std::wstring& key, const std::wstring& line) {
    std::vector<std::wstring>& lines = histories[key];
    lines.push_back(line);
    if (lines.size() > 240) {
        lines.erase(lines.begin(), lines.begin() + 40);
    }
    if (key == active_key) {
        render_active_history();
    }
}

// 创建当前窗口的日志文件。
void open_event_log() {
    wchar_t module[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, module, static_cast<DWORD>(std::size(module)));
    if (length == 0 || length >= std::size(module)) {
        return;
    }
    std::error_code error;
    const auto directory = std::filesystem::path(module).parent_path().parent_path() /
                           L"evidence" / L"logs" / L"gui_sessions";
    std::filesystem::create_directories(directory, error);
    if (!error) {
        event_log_path = directory / (L"client_" + std::to_wstring(GetCurrentProcessId()) + L".log");
    }
}

// 追加动态。
void append_activity(const std::wstring& line) {
    const bool dated = line.size() > 5 && line[0] == L'[' && line[5] == L'-';
    activity_lines.push_back(dated ? line : L"[" + from_utf8(chat::local_timestamp()) + L"] " + line);
    if (activity_lines.size() > 180) {
        activity_lines.erase(activity_lines.begin(), activity_lines.begin() + 30);
    }
    render_activity_log();
    if (!event_log_path.empty()) {
        std::ofstream file(event_log_path, std::ios::app | std::ios::binary);
        file << to_utf8(activity_lines.back()) << "\r\n";
    }
}

// 记录收到的聊天消息。
void record_received(const std::string& sender, const std::string& seq,
                     const std::string& type, std::size_t bytes) {
    ++messages_received;
    std::uint64_t number = 0;
    if (sender == current_user && chat::parse_sequence(seq, number) && awaiting_echo.erase(number)) {
        ++messages_confirmed;
    }
    append_activity(L"接收 " + from_utf8(type) + L" | " + from_utf8(sender) +
                    L" | 序号 " + from_utf8(seq) + L" | 正文 " + std::to_wstring(bytes) + L" 字节");
    update_controls();
}

// 整理公共聊天室动态。
std::wstring format_room_notice(const std::string& timestamp, const std::string& detail) {
    const std::string joined_suffix = " joined the room";
    const std::string left_suffix = " left the room";
    const std::wstring time = L"[" + from_utf8(timestamp) + L"] ";
    if (detail.size() > joined_suffix.size() &&
        detail.compare(detail.size() - joined_suffix.size(), joined_suffix.size(), joined_suffix) == 0) {
        return time + from_utf8(detail.substr(0, detail.size() - joined_suffix.size())) +
               L" 进入公共聊天室";
    }
    if (detail.size() > left_suffix.size() &&
        detail.compare(detail.size() - left_suffix.size(), left_suffix.size(), left_suffix) == 0) {
        return time + from_utf8(detail.substr(0, detail.size() - left_suffix.size())) +
               L" 离开公共聊天室";
    }
    return time + from_utf8(detail);
}

// 整理群组动态。
std::wstring format_group_notice(const std::string& timestamp, const std::string& group,
                                 const std::string& detail) {
    const std::wstring time = L"[" + from_utf8(timestamp) + L"] ";
    const std::string created = " created and joined the group";
    const std::string joined_suffix = " joined the group";
    const std::string left_suffix = " left the group";
    const std::size_t added = detail.find(" added ");
    if (detail.size() > created.size() &&
        detail.compare(detail.size() - created.size(), created.size(), created) == 0) {
        return time + from_utf8(detail.substr(0, detail.size() - created.size())) +
               L" 建立群聊 " + from_utf8(group) + L" 并加入";
    }
    if (detail.size() > joined_suffix.size() &&
        detail.compare(detail.size() - joined_suffix.size(), joined_suffix.size(), joined_suffix) == 0) {
        return time + from_utf8(detail.substr(0, detail.size() - joined_suffix.size())) +
               L" 进入群聊 " + from_utf8(group);
    }
    if (detail.size() > left_suffix.size() &&
        detail.compare(detail.size() - left_suffix.size(), left_suffix.size(), left_suffix) == 0) {
        return time + from_utf8(detail.substr(0, detail.size() - left_suffix.size())) +
               L" 离开群聊 " + from_utf8(group);
    }
    if (added != std::string::npos) {
        return time + from_utf8(detail.substr(0, added)) + L" 将 " +
               from_utf8(detail.substr(added + 7)) + L" 加入群聊 " + from_utf8(group);
    }
    return time + from_utf8(detail) + L"（" + from_utf8(group) + L"）";
}

// 设置会话标题。
void update_conversation_header() {
    std::wstring title = L"公共聊天室";
    if (active_kind == ConversationKind::direct_chat) {
        title = L"私聊 · " + active_key.substr(7);
    } else if (active_kind == ConversationKind::group_chat) {
        const std::wstring group = active_key.substr(6);
        title = group.empty() ? L"群聊" : L"群聊 · " + group;
    }
    SetWindowTextW(conversation_title, title.c_str());
}

// 更新页面控件状态。
void update_controls() {
    const bool online = connected.load();
    const bool ready = online && joined && !disconnecting;
    const bool has_group = active_kind != ConversationKind::group_chat ||
                           active_key.size() > std::wstring(L"group:").size();

    EnableWindow(connect_button, !online);
    EnableWindow(ip_input, !online);
    EnableWindow(port_input, !online);
    EnableWindow(name_input, !online);
    EnableWindow(refresh_button, ready);
    EnableWindow(quit_button, online && !disconnecting);
    EnableWindow(conversation_list, ready);
    EnableWindow(group_list, ready);
    EnableWindow(online_user_list, ready);
    EnableWindow(group_name_input, ready);
    EnableWindow(create_group_button, ready);
    EnableWindow(join_group_button, ready && active_kind == ConversationKind::group_chat);
    EnableWindow(leave_group_button, ready && active_kind == ConversationKind::group_chat);
    EnableWindow(message_input, ready && has_group);
    EnableWindow(send_button, ready && has_group);

    std::wstring status = L"离线";
    if (online && !joined) {
        status = L"正在登录  |  " + server_address;
    } else if (ready) {
        status = L"在线  |  " + server_address + L"  |  " + from_utf8(current_user) +
                 L"  |  在线 " + std::to_wstring(online_users.size()) + L" 人";
    }
    if (online || messages_sent || messages_received) {
        status += L"  |  发 " + std::to_wstring(messages_sent) +
                  L" 收 " + std::to_wstring(messages_received) +
                  L" 回显 " + std::to_wstring(messages_confirmed) +
                  L" 待回显 " + std::to_wstring(awaiting_echo.size());
    }
    if (disconnecting) {
        status = L"正在断开服务器连接";
    }
    SetWindowTextW(status_label, status.c_str());

    InvalidateRect(main_window, nullptr, FALSE);
}

// 记录已经打开的私聊会话。
void remember_direct_contact(const std::string& user) {
    if (user.empty() || user == current_user ||
        std::find(direct_contacts.begin(), direct_contacts.end(), user) != direct_contacts.end()) {
        return;
    }
    direct_contacts.push_back(user);
    std::sort(direct_contacts.begin(), direct_contacts.end());
}

// 重建聊天页的会话列表。
void rebuild_conversation_list() {
    if (conversation_list == nullptr) {
        return;
    }

    conversation_entries.clear();
    SendMessageW(conversation_list, LB_RESETCONTENT, 0, 0);

    conversation_entries.push_back(
        {ConversationKind::public_room, L"public", L"公共聊天室"});

    for (const std::string& user : direct_contacts) {
        const std::wstring wide_user = from_utf8(user);
        conversation_entries.push_back(
            {ConversationKind::direct_chat, direct_key(wide_user), wide_user});
    }

    int selected = 0;
    for (std::size_t i = 0; i < conversation_entries.size(); ++i) {
        const ConversationEntry& entry = conversation_entries[i];
        const LRESULT position = SendMessageW(
            conversation_list, LB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(entry.title.c_str()));
        SendMessageW(conversation_list, LB_SETITEMDATA,
                     static_cast<WPARAM>(position), static_cast<LPARAM>(i));
        if (entry.kind == active_kind && entry.key == active_key) {
            selected = static_cast<int>(i);
        }
    }
    SendMessageW(conversation_list, LB_SETCURSEL, selected, 0);
}

// 重建在线用户列表。
void rebuild_online_user_list() {
    if (online_user_list == nullptr) {
        return;
    }

    online_user_entries = online_users;
    std::sort(online_user_entries.begin(), online_user_entries.end());
    SendMessageW(online_user_list, LB_RESETCONTENT, 0, 0);
    for (std::size_t i = 0; i < online_user_entries.size(); ++i) {
        const std::wstring name = from_utf8(online_user_entries[i]);
        const std::wstring item = online_user_entries[i] == current_user
                                      ? name + L"（当前账号）" : name;
        const LRESULT position = SendMessageW(
            online_user_list, LB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(item.c_str()));
        SendMessageW(online_user_list, LB_SETITEMDATA,
                     static_cast<WPARAM>(position), static_cast<LPARAM>(i));
    }
}

// 重建群聊页的群组列表。
void rebuild_group_list() {
    if (group_list == nullptr) {
        return;
    }

    std::sort(group_names.begin(), group_names.end());
    SendMessageW(group_list, LB_RESETCONTENT, 0, 0);
    int selected = LB_ERR;
    for (std::size_t i = 0; i < group_names.size(); ++i) {
        const std::wstring group = from_utf8(group_names[i]);
        const std::wstring item = group;
        const LRESULT position = SendMessageW(
            group_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
        SendMessageW(group_list, LB_SETITEMDATA,
                     static_cast<WPARAM>(position), static_cast<LPARAM>(i));
        if (active_kind == ConversationKind::group_chat &&
            active_key == group_key(group)) {
            selected = static_cast<int>(i);
        }
    }
    if (selected != LB_ERR) {
        SendMessageW(group_list, LB_SETCURSEL, selected, 0);
    }
}

// 选择右侧会话。
void set_active_conversation(ConversationKind kind, const std::wstring& key) {
    active_kind = kind;
    active_key = key;
    update_conversation_header();
    render_active_history();
    rebuild_conversation_list();
    rebuild_group_list();
    rebuild_online_user_list();
    update_controls();
}

// 切换聊天页或群聊页。
void switch_view(ViewPage page) {
    active_view = page;
    const bool show_groups = page == ViewPage::groups;

    if (show_groups && active_kind != ConversationKind::group_chat) {
        active_kind = ConversationKind::group_chat;
        active_key = L"group:";
        update_conversation_header();
        render_active_history();
    }
    if (!show_groups && active_kind == ConversationKind::group_chat) {
        active_kind = ConversationKind::public_room;
        active_key = L"public";
        update_conversation_header();
        render_active_history();
    }

    ShowWindow(conversation_list, show_groups ? SW_HIDE : SW_SHOW);
    ShowWindow(group_list, show_groups ? SW_SHOW : SW_HIDE);
    ShowWindow(group_name_input, show_groups ? SW_SHOW : SW_HIDE);
    ShowWindow(create_group_button, show_groups ? SW_SHOW : SW_HIDE);
    ShowWindow(join_group_button, show_groups ? SW_SHOW : SW_HIDE);
    ShowWindow(leave_group_button, show_groups ? SW_SHOW : SW_HIDE);
    SetWindowTextW(sidebar_title, show_groups ? L"群组" : L"会话");
    SetWindowTextW(online_title, show_groups ? L"选择成员" : L"在线用户");

    RECT client{};
    GetClientRect(main_window, &client);
    layout_controls(client.right - client.left, client.bottom - client.top);
    InvalidateRect(main_window, nullptr, FALSE);
    update_controls();
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

// 向服务端请求用户和群组列表。
void refresh_remote_lists() {
    if (!connected.load() || !joined) {
        return;
    }
    if (!chat::send_frame(server_socket, "WHO")) {
        append_activity(L"[错误] 在线用户请求发送失败。");
    }
    if (!chat::send_frame(server_socket, "GROUP|LIST")) {
        append_activity(L"[错误] 群组列表请求发送失败。");
    }
}

// 建立 TCP 连接并启动接收线程。
void connect_to_server() {
    const std::wstring wide_ip = get_control_text(ip_input);
    const std::wstring wide_port = get_control_text(port_input);
    const std::string ip = to_utf8(wide_ip);
    const std::string port_text = to_utf8(wide_port);
    const std::string user = to_utf8(get_control_text(name_input));

    if (!chat::is_valid_name(user)) {
        append_activity(L"[错误] 用户名使用 1-16 位英文字母、数字或下划线。");
        return;
    }

    unsigned short port = 0;
    if (!parse_port_number(port_text, port)) {
        append_activity(L"[错误] 端口填写 1-65535 的整数。");
        return;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1) {
        append_activity(L"[错误] 服务器地址填写 IPv4 地址。");
        return;
    }

    SOCKET connection = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (connection == INVALID_SOCKET) {
        append_activity(L"[错误] socket() 创建失败。");
        return;
    }

    if (::connect(connection, reinterpret_cast<sockaddr*>(&address),
                  sizeof(address)) == SOCKET_ERROR) {
        append_activity(L"[错误] connect() 失败，请检查服务端和端口。");
        closesocket(connection);
        return;
    }

    if (!chat::send_frame(connection, "JOIN|" + user)) {
        append_activity(L"[错误] JOIN 帧发送失败。");
        closesocket(connection);
        return;
    }

    server_socket = connection;
    server_address = wide_ip + L":" + wide_port;
    current_user = user;
    sequence = 1;
    messages_sent = 0;
    messages_received = 0;
    messages_confirmed = 0;
    awaiting_echo.clear();
    disconnecting = false;
    joined = false;
    connected.store(true);
    online_users.clear();
    group_names.clear();
    direct_contacts.clear();
    online_user_entries.clear();
    histories.clear();
    activity_lines.clear();
    active_kind = ConversationKind::public_room;
    active_key = L"public";
    switch_view(ViewPage::chats);
    rebuild_online_user_list();
    rebuild_group_list();
    update_conversation_header();
    render_active_history();
    render_activity_log();
    append_activity(L"已连接 " + from_utf8(ip) + L":" + from_utf8(port_text) +
                    L"，登录 " + from_utf8(user));
    update_controls();

    receive_thread = CreateThread(
        nullptr, 0,
        [](LPVOID parameter) -> DWORD {
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
                    if (!PostMessageW(main_window, WM_CHAT_FRAME, 0,
                                      reinterpret_cast<LPARAM>(copy))) {
                        delete copy;
                    }
                }
            }

            PostMessageW(main_window, WM_CHAT_DISCONNECTED, 0, 0);
            return 0;
        },
        reinterpret_cast<LPVOID>(static_cast<UINT_PTR>(connection)), 0, nullptr);

    if (receive_thread == nullptr) {
        append_activity(L"[错误] 接收线程创建失败。");
        shutdown(connection, SD_BOTH);
        closesocket(connection);
        server_socket = INVALID_SOCKET;
        connected.store(false);
        update_controls();
    }
}

// 显示服务端协议帧。
void display_protocol_frame(const std::string& frame) {
    if (frame.rfind("WELCOME|", 0) == 0) {
        const auto fields = chat::split_fields(frame, 2);
        if (fields.size() == 3) {
            current_user = fields[1];
            joined = true;
            const std::wstring title = L"网络聊天室 - " + from_utf8(fields[1]);
            SetWindowTextW(main_window, title.c_str());
            append_activity(L"已登录 " + from_utf8(fields[1]) +
                            L"，服务端时间 " + from_utf8(fields[2]));
            refresh_remote_lists();
        }
        update_controls();
        return;
    }

    if (frame.rfind("MSG|", 0) == 0) {
        const auto fields = chat::split_fields(frame, 4);
        if (fields.size() == 5) {
            record_received(fields[2], fields[3], "MSG", fields[4].size());
            append_history(L"public", L"[" + from_utf8(fields[1]) + L"] " +
                           from_utf8(fields[2]) + L"：" + from_utf8(fields[4]));
        }
        return;
    }

    if (frame.rfind("PM|", 0) == 0) {
        const auto fields = chat::split_fields(frame, 5);
        if (fields.size() == 6) {
            record_received(fields[2], fields[4], "PM", fields[5].size());
            const std::string peer = fields[2] == current_user ? fields[3] : fields[2];
            const std::wstring key = direct_key(from_utf8(peer));
            remember_direct_contact(peer);
            append_history(key, L"[" + from_utf8(fields[1]) + L"] " +
                           from_utf8(fields[2]) + L"：" + from_utf8(fields[5]));
            rebuild_conversation_list();
        }
        return;
    }

    if (frame.rfind("GMSG|", 0) == 0) {
        const auto fields = chat::split_fields(frame, 5);
        if (fields.size() == 6) {
            record_received(fields[3], fields[4], "GMSG", fields[5].size());
            const std::wstring key = group_key(from_utf8(fields[2]));
            append_history(key, L"[" + from_utf8(fields[1]) + L"] " +
                           from_utf8(fields[3]) + L"：" + from_utf8(fields[5]));
        }
        return;
    }

    if (frame.rfind("GNOTICE|", 0) == 0) {
        const auto fields = chat::split_fields(frame, 3);
        if (fields.size() == 4) {
            append_activity(format_group_notice(fields[1], fields[2], fields[3]));
            refresh_remote_lists();
        }
        return;
    }

    if (frame.rfind("GROUP_OK|", 0) == 0) {
        const auto fields = chat::split_fields(frame, 3);
        if (fields.size() == 3) {
            const std::wstring group = from_utf8(fields[2]);
            if (fields[1] == "LEAVE") {
                append_activity(L"已退出群聊 " + group);
            }
            if (fields[1] == "CREATE" || fields[1] == "JOIN") {
                switch_view(ViewPage::groups);
                set_active_conversation(ConversationKind::group_chat, group_key(group));
            }
            refresh_remote_lists();
        } else if (fields.size() == 4 && fields[1] == "ADD") {
            refresh_remote_lists();
        }
        return;
    }

    if (frame.rfind("GROUPS|", 0) == 0) {
        group_names = split_csv(frame.substr(7));
        rebuild_group_list();
        return;
    }

    if (frame.rfind("NOTICE|", 0) == 0) {
        const auto fields = chat::split_fields(frame, 2);
        if (fields.size() == 3) {
            append_activity(format_room_notice(fields[1], fields[2]));
            refresh_remote_lists();
        }
        return;
    }

    if (frame.rfind("USERS|", 0) == 0) {
        online_users = split_csv(frame.substr(6));
        rebuild_online_user_list();
        update_controls();
        return;
    }

    if (frame.rfind("ERR|", 0) == 0) {
        append_activity(L"[错误] " + from_utf8(frame.substr(4)));
        return;
    }

    if (frame.rfind("BYE|", 0) == 0) {
        append_activity(L"服务器已确认退出");
        return;
    }

    append_activity(L"[协议] " + from_utf8(frame));
}

// 发送当前会话中的消息。
void send_message() {
    if (!connected.load() || !joined || disconnecting) {
        return;
    }

    const std::string body = to_utf8(get_control_text(message_input));
    if (!chat::is_valid_body(body)) {
        append_activity(L"[提示] 输入 1-4096 字节的单行消息。");
        return;
    }

    std::string frame;
    if (active_kind == ConversationKind::public_room) {
        frame = "TEXT|" + std::to_string(sequence) + "|" + body;
    } else if (active_kind == ConversationKind::direct_chat) {
        const std::wstring peer = active_key.substr(std::wstring(L"direct:").size());
        frame = "PM|" + std::to_string(sequence) + "|" + to_utf8(peer) + "|" + body;
    } else {
        const std::wstring group = active_key.substr(std::wstring(L"group:").size());
        if (group.empty()) {
            append_activity(L"[提示] 先在群聊页选择一个群组。");
            return;
        }
        frame = "GTEXT|" + std::to_string(sequence) + "|" + to_utf8(group) + "|" + body;
    }

    if (!chat::send_frame(server_socket, frame)) {
        append_activity(L"[错误] 消息发送失败。");
        return;
    }

    ++messages_sent;
    awaiting_echo.insert(sequence);
    append_activity(L"发送 " + from_utf8(frame.substr(0, frame.find('|'))) +
                    L" | 序号 " + std::to_wstring(sequence) +
                    L" | 正文 " + std::to_wstring(body.size()) + L" 字节");
    ++sequence;
    update_controls();
    SetWindowTextW(message_input, L"");
    SetFocus(message_input);
}

// 发送群组操作帧。
void send_group_action(const std::string& action) {
    if (!connected.load() || !joined) {
        return;
    }

    std::string group = to_utf8(get_control_text(group_name_input));
    if (action == "LEAVE") {
        group = to_utf8(active_key.substr(std::wstring(L"group:").size()));
    }
    if (!chat::is_valid_name(group)) {
        append_activity(L"[提示] 群名使用 1-16 位英文字母、数字或下划线。");
        return;
    }

    if (!chat::send_frame(server_socket, "GROUP|" + action + "|" + group)) {
        append_activity(L"[错误] 群组操作发送失败。");
        return;
    }

    const std::wstring group_wide = from_utf8(group);
    if (action == "CREATE") {
        set_active_conversation(ConversationKind::group_chat, group_key(group_wide));
    }
    SetWindowTextW(group_name_input, L"");
}

// 将选中的在线用户加入当前群组。
void add_selected_group_members() {
    if (!connected.load() || !joined || active_kind != ConversationKind::group_chat) {
        return;
    }

    const std::string group = to_utf8(active_key.substr(std::wstring(L"group:").size()));
    if (!chat::is_valid_name(group)) {
        append_activity(L"[提示] 先选择一个群组。");
        return;
    }

    const LRESULT count = SendMessageW(online_user_list, LB_GETSELCOUNT, 0, 0);
    if (count <= 0) {
        append_activity(L"[提示] 在左侧选择要加入群聊的用户。");
        return;
    }

    std::vector<int> selected(static_cast<std::size_t>(count));
    SendMessageW(online_user_list, LB_GETSELITEMS, count,
                 reinterpret_cast<LPARAM>(selected.data()));
    for (int position : selected) {
        const LRESULT index = SendMessageW(online_user_list, LB_GETITEMDATA, position, 0);
        if (index == LB_ERR || static_cast<std::size_t>(index) >= online_user_entries.size()) {
            continue;
        }
        const std::string& user = online_user_entries[static_cast<std::size_t>(index)];
        if (user == current_user) {
            continue;
        }
        if (!chat::send_frame(server_socket, "GROUP|ADD|" + group + "|" + user)) {
            append_activity(L"[错误] 添加群成员发送失败。");
            return;
        }
    }
    SendMessageW(online_user_list, LB_SETSEL, FALSE, static_cast<LPARAM>(-1));
}

// 请求退出并关闭写方向。
void quit_chat() {
    if (!connected.load() || disconnecting) {
        return;
    }

    disconnecting = true;
    append_activity(L"发送 QUIT，正在断开服务器连接");
    if (chat::send_frame(server_socket, "QUIT")) {
        shutdown(server_socket, SD_SEND);
    } else {
        shutdown(server_socket, SD_BOTH);
    }
    SetTimer(main_window, ID_DISCONNECT_TIMER, 2000, nullptr);
    update_controls();
}

// 自动演示写入真实界面。
void run_demo_step() {
    if (!connected.load()) {
        connect_to_server();
        return;
    }
    if (!joined) {
        return;
    }

    const wchar_t* text = nullptr;
    if (demo_role == L"Alice" || demo_role == L"DemoAlice") {
        static const wchar_t* steps[] = {
            L"大家好，我是 Alice。", L"/"};
        if (demo_step == 0) {
            set_active_conversation(ConversationKind::public_room, L"public");
            text = steps[0];
        } else if (demo_step == 1) {
            SetWindowTextW(group_name_input, L"Study");
            switch_view(ViewPage::groups);
            send_group_action("CREATE");
        }
    } else if (demo_role == L"Bob" || demo_role == L"DemoBob") {
        if (demo_step == 1) {
            SetWindowTextW(group_name_input, L"Study");
            switch_view(ViewPage::groups);
            send_group_action("JOIN");
        }
    } else if ((demo_role == L"Charlie" || demo_role == L"DemoCharlie") && demo_step == 0) {
        set_active_conversation(ConversationKind::public_room, L"public");
        text = L"Charlie 已上线。";
    }

    if (text != nullptr) {
        SetWindowTextW(message_input, text);
        send_message();
    }
    ++demo_step;
    if (demo_step >= 3) {
        KillTimer(main_window, ID_DEMO_TIMER);
    }
}

// 释放已创建的字体。
void delete_font(HFONT& font) {
    if (font != nullptr) {
        DeleteObject(font);
        font = nullptr;
    }
}

// 按当前 DPI 创建界面字体。
void rebuild_fonts() {
    delete_font(regular_font);
    delete_font(medium_font);
    delete_font(title_font);
    delete_font(small_font);

    regular_font = CreateFontW(-scale(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                               L"Microsoft YaHei UI");
    medium_font = CreateFontW(-scale(15), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                              L"Microsoft YaHei UI");
    title_font = CreateFontW(-scale(20), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                             L"Microsoft YaHei UI");
    small_font = CreateFontW(-scale(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                             L"Microsoft YaHei UI");
}

// 将字体应用到已经创建的控件。
void apply_fonts() {
    const HWND controls[] = {
        brand_label, server_label, port_label, name_label,
        ip_input, port_input, name_input, connect_button, refresh_button,
        quit_button, status_label, nav_chat_button, nav_group_button,
        sidebar_title, online_title, conversation_list, group_list, online_user_list, conversation_title,
        activity_title, group_name_input, create_group_button,
        join_group_button, leave_group_button, chat_log, activity_log, message_input, send_button,
    };
    for (HWND control : controls) {
        if (control != nullptr) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(regular_font), TRUE);
        }
    }

    for (HWND control : {brand_label, conversation_title}) {
        if (control != nullptr) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(title_font), TRUE);
        }
    }
    for (HWND control : {status_label}) {
        if (control != nullptr) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(small_font), TRUE);
        }
    }
    if (activity_log != nullptr) {
        SendMessageW(activity_log, WM_SETFONT, reinterpret_cast<WPARAM>(small_font), TRUE);
    }
    for (HWND control : {sidebar_title, online_title, activity_title}) {
        if (control != nullptr) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(medium_font), TRUE);
        }
    }
    for (HWND control : {conversation_list, group_list, online_user_list}) {
        if (control != nullptr) {
            SendMessageW(control, LB_SETITEMHEIGHT, 0, scale(32));
        }
    }
}

// 创建标准 Win32 控件。
HWND make_control(const wchar_t* class_name, const wchar_t* text, DWORD style,
                  DWORD extended_style, int identifier, HWND parent) {
    HWND control = CreateWindowExW(
        extended_style, class_name, text, style | WS_CHILD | WS_VISIBLE,
        0, 0, 80, 28, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(identifier)),
        GetModuleHandleW(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(regular_font), TRUE);
    return control;
}

// 绘制圆角矩形。
void fill_round_rect(HDC device, const RECT& rect, COLORREF color, int radius) {
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ old_brush = SelectObject(device, brush);
    HGDIOBJ old_pen = SelectObject(device, pen);
    RoundRect(device, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(device, old_pen);
    SelectObject(device, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);
}

// 绘制自定义按钮。
void draw_button(const DRAWITEMSTRUCT& item) {
    const int identifier = static_cast<int>(item.CtlID);
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool selected_nav =
        (identifier == ID_NAV_CHAT && active_view == ViewPage::chats) ||
        (identifier == ID_NAV_GROUP && active_view == ViewPage::groups);

    COLORREF background = COLOR_CARD;
    COLORREF foreground = COLOR_TEXT;
    if (identifier == ID_CONNECT || identifier == ID_SEND ||
        identifier == ID_CREATE_GROUP || identifier == ID_ADD_GROUP_MEMBER) {
        background = COLOR_PRIMARY;
        foreground = RGB(255, 255, 255);
    }
    if (identifier == ID_QUIT || identifier == ID_LEAVE_GROUP) {
        background = COLOR_DANGER;
        foreground = RGB(255, 255, 255);
    }
    if (identifier == ID_NAV_CHAT || identifier == ID_NAV_GROUP) {
        background = selected_nav ? COLOR_PRIMARY : COLOR_HEADER;
        foreground = selected_nav ? RGB(255, 255, 255) : COLOR_HEADER_MUTED;
    }
    if (identifier == ID_REFRESH) {
        background = RGB(226, 232, 240);
        foreground = COLOR_TEXT;
    }
    if (disabled) {
        background = RGB(226, 232, 240);
        foreground = RGB(148, 163, 184);
    } else if (pressed && identifier != ID_NAV_CHAT && identifier != ID_NAV_GROUP) {
        background = identifier == ID_QUIT || identifier == ID_LEAVE_GROUP
                         ? RGB(185, 28, 28)
                         : COLOR_PRIMARY_DARK;
    }

    RECT rect = item.rcItem;
    HBRUSH parent_background = CreateSolidBrush(
        identifier == ID_NAV_CHAT || identifier == ID_NAV_GROUP ? COLOR_HEADER : COLOR_CARD);
    FillRect(item.hDC, &rect, parent_background);
    DeleteObject(parent_background);
    fill_round_rect(item.hDC, rect, background, scale(6));
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, foreground);
    SelectObject(item.hDC, medium_font);
    wchar_t text[128]{};
    GetWindowTextW(item.hwndItem, text, static_cast<int>(std::size(text)));
    DrawTextW(item.hDC, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if ((item.itemState & ODS_FOCUS) != 0) {
        InflateRect(&rect, -scale(3), -scale(3));
        DrawFocusRect(item.hDC, &rect);
    }
}

// 绘制会话和群组列表项。
void draw_list_item(const DRAWITEMSTRUCT& item) {
    if (item.itemID == static_cast<UINT>(-1)) {
        return;
    }

    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    RECT rect = item.rcItem;
    HBRUSH back = CreateSolidBrush(selected ? COLOR_PRIMARY_LIGHT : COLOR_CARD);
    FillRect(item.hDC, &rect, back);
    DeleteObject(back);

    wchar_t text[160]{};
    SendMessageW(item.hwndItem, LB_GETTEXT, item.itemID,
                 reinterpret_cast<LPARAM>(text));
    rect.left += scale(14);
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, selected ? COLOR_PRIMARY_DARK : COLOR_TEXT);
    SelectObject(item.hDC, medium_font);
    DrawTextW(item.hDC, text, -1, &rect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

// 计算连接栏、会话区和底部动态区。
struct PanelLayout {
    RECT connection;
    RECT sidebar;
    RECT conversation;
    RECT activity;
    bool compact;
};

PanelLayout calculate_panels(int width, int height) {
    const int margin = scale(14);
    const int gap = scale(12);
    const bool compact = width < scale(1000);
    const int connection_top = scale(60);
    const int connection_bottom = connection_top + scale(compact ? 88 : 50);
    const int middle_top = connection_bottom + gap;
    const int activity_height = std::clamp((height - middle_top) * 3 / 10,
                                           scale(146), scale(210));
    const int activity_top = height - margin - activity_height;
    const int middle_bottom = activity_top - gap;
    const int sidebar_right = margin + std::clamp(width / 5, scale(184), scale(224));
    return {
        {margin, connection_top, width - margin, connection_bottom},
        {margin, middle_top, sidebar_right, middle_bottom},
        {sidebar_right + gap, middle_top, width - margin, middle_bottom},
        {margin, activity_top, width - margin, height - margin},
        compact,
    };
}

// 布置窗口控件。
void layout_controls(int width, int height) {
    const auto panels = calculate_panels(width, height);
    const int padding = scale(12);
    const int field_height = scale(30);
    const int x = panels.connection.left + padding;
    const int y = panels.connection.top + scale(10);
    const int actions = panels.connection.right - padding - scale(266);
    auto place = [](HWND control, int left, int top, int w, int h) {
        MoveWindow(control, left, top, std::max(1, w), std::max(1, h), TRUE);
    };

    place(brand_label, scale(20), scale(10), scale(220), scale(28));
    place(nav_chat_button, width - scale(170), scale(9), scale(68), scale(30));
    place(nav_group_button, width - scale(92), scale(9), scale(68), scale(30));

    place(server_label, x, y + scale(4), scale(48), scale(22));
    place(ip_input, x + scale(50), y, scale(134), field_height);
    place(port_label, x + scale(196), y + scale(4), scale(32), scale(22));
    place(port_input, x + scale(234), y, scale(66), field_height);
    const int name_y = panels.compact ? y + scale(38) : y;
    const int name_x = panels.compact ? x : x + scale(314);
    place(name_label, name_x, name_y + scale(4), scale(34), scale(22));
    place(name_input, name_x + scale(40), name_y,
          actions - name_x - scale(52), field_height);
    place(connect_button, actions, name_y, scale(110), field_height);
    place(quit_button, actions + scale(118), name_y, scale(88), field_height);
    place(refresh_button, actions + scale(214), name_y, scale(52), field_height);

    const RECT& side = panels.sidebar;
    const int first_top = side.top + scale(36);
    const int available = side.bottom - padding - first_top - scale(34);
    const int first_height = available * 2 / 5;
    const int second_title_y = first_top + first_height + scale(10);
    const int second_top = second_title_y + scale(24);
    place(sidebar_title, side.left + padding, side.top + scale(10),
          side.right - side.left - 2 * padding, scale(22));
    place(conversation_list, side.left + padding, first_top,
          side.right - side.left - 2 * padding, first_height);
    place(group_list, side.left + padding, first_top,
          side.right - side.left - 2 * padding, first_height);
    place(online_title, side.left + padding, second_title_y,
          side.right - side.left - 2 * padding, scale(22));
    place(online_user_list, side.left + padding, second_top,
          side.right - side.left - 2 * padding, side.bottom - padding - second_top);

    const RECT& chat = panels.conversation;
    const int content_x = chat.left + padding;
    const int content_width = chat.right - chat.left - 2 * padding;
    place(conversation_title, content_x, chat.top + scale(10), content_width, scale(28));
    const int group_y = chat.top + scale(44);
    const int group_actions = chat.right - padding - scale(260);
    place(group_name_input, content_x, group_y, group_actions - content_x - scale(8), field_height);
    place(create_group_button, group_actions, group_y, scale(84), field_height);
    place(join_group_button, group_actions + scale(90), group_y, scale(84), field_height);
    place(leave_group_button, group_actions + scale(180), group_y, scale(80), field_height);

    const int chat_top = chat.top + scale(active_view == ViewPage::groups ? 84 : 46);
    const int compose_top = chat.bottom - padding - scale(34);
    place(chat_log, content_x, chat_top, content_width, compose_top - scale(8) - chat_top);
    place(message_input, content_x, compose_top, content_width - scale(86), scale(34));
    place(send_button, chat.right - padding - scale(78), compose_top, scale(78), scale(34));

    const RECT& activity = panels.activity;
    place(activity_title, activity.left + padding, activity.top + scale(10), scale(82), scale(22));
    place(status_label, activity.left + scale(106), activity.top + scale(12),
          activity.right - activity.left - scale(118), scale(20));
    place(activity_log, activity.left + padding, activity.top + scale(40),
          activity.right - activity.left - 2 * padding, activity.bottom - activity.top - scale(52));
}

// 绘制分区背景。
void paint_window(HWND window) {
    PAINTSTRUCT paint{};
    HDC device = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    FillRect(device, &client, canvas_brush);
    RECT header{0, 0, client.right, scale(48)};
    FillRect(device, &header, header_brush);
    const auto panels = calculate_panels(client.right, client.bottom);
    for (const RECT& rect : {panels.connection, panels.sidebar, panels.conversation, panels.activity}) {
        fill_round_rect(device, rect, COLOR_CARD, scale(8));
    }
    EndPaint(window, &paint);
}

// 处理窗口消息。
LRESULT CALLBACK window_procedure(HWND window, UINT message,
                                  WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        main_window = window;
        current_dpi = get_window_dpi(window);
        rebuild_fonts();
        canvas_brush = CreateSolidBrush(COLOR_CANVAS);
        card_brush = CreateSolidBrush(COLOR_CARD);
        header_brush = CreateSolidBrush(COLOR_HEADER);
        activity_brush = CreateSolidBrush(COLOR_ACTIVITY);

        brand_label = make_control(L"STATIC", L"网络聊天室", SS_LEFT, 0, 0, window);
        nav_chat_button = make_control(
            L"BUTTON", L"聊天", BS_OWNERDRAW | WS_TABSTOP, 0, ID_NAV_CHAT, window);
        nav_group_button = make_control(
            L"BUTTON", L"群组", BS_OWNERDRAW | WS_TABSTOP, 0, ID_NAV_GROUP, window);

        server_label = make_control(L"STATIC", L"服务器", SS_LEFT, 0, 0, window);
        ip_input = make_control(L"EDIT", L"127.0.0.1",
                                WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
                                0, 0, window);
        port_label = make_control(L"STATIC", L"端口", SS_LEFT, 0, 0, window);
        port_input = make_control(L"EDIT", L"8888",
                                  WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
                                  0, 0, window);
        name_label = make_control(L"STATIC", L"昵称", SS_LEFT, 0, 0, window);
        const std::wstring default_name = requested_name.empty()
                                      ? L"User" + std::to_wstring(GetCurrentProcessId())
                                      : requested_name;
        name_input = make_control(L"EDIT", default_name.c_str(),
                                  WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
                                  0, 0, window);
        connect_button = make_control(
            L"BUTTON", L"连接服务器", BS_OWNERDRAW | WS_TABSTOP, 0, ID_CONNECT, window);
        refresh_button = make_control(
            L"BUTTON", L"刷新", BS_OWNERDRAW | WS_TABSTOP, 0, ID_REFRESH, window);
        quit_button = make_control(
            L"BUTTON", L"断开连接", BS_OWNERDRAW | WS_TABSTOP, 0, ID_QUIT, window);
        status_label = make_control(L"STATIC", L"离线",
                                    SS_LEFT | SS_ENDELLIPSIS, 0, 0, window);

        sidebar_title = make_control(L"STATIC", L"会话", SS_LEFT, 0, 0, window);
        conversation_list = make_control(
            L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS |
                                  LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT,
            0, ID_CONVERSATIONS, window);
        group_list = make_control(
            L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS |
                                  LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT,
            0, ID_GROUP_LIST, window);
        online_title = make_control(L"STATIC", L"在线用户", SS_LEFT, 0, 0, window);
        online_user_list = make_control(
            L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS |
                                  LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT |
                                  LBS_EXTENDEDSEL | WS_TABSTOP,
            0, ID_ONLINE_USERS, window);

        conversation_title = make_control(L"STATIC", L"公共聊天室", SS_LEFT, 0, 0, window);
        activity_title = make_control(L"STATIC", L"连接动态", SS_LEFT, 0, 0, window);

        group_name_input = make_control(
            L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
            0, ID_GROUP_NAME, window);
        create_group_button = make_control(
            L"BUTTON", L"建立群聊", BS_OWNERDRAW | WS_TABSTOP, 0, ID_CREATE_GROUP, window);
        join_group_button = make_control(
            L"BUTTON", L"添加成员", BS_OWNERDRAW | WS_TABSTOP, 0, ID_ADD_GROUP_MEMBER, window);
        leave_group_button = make_control(
            L"BUTTON", L"退出群聊", BS_OWNERDRAW | WS_TABSTOP, 0, ID_LEAVE_GROUP, window);

        chat_log = make_control(
            L"EDIT", L"",
            WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
            0, 0, window);
        activity_log = make_control(
            L"EDIT", L"",
            WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
            0, 0, window);
        message_input = make_control(
            L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
            0, 0, window);
        send_button = make_control(
            L"BUTTON", L"发送", BS_OWNERDRAW | WS_TABSTOP, 0, ID_SEND, window);

        SendMessageW(message_input, EM_SETCUEBANNER, FALSE,
                     reinterpret_cast<LPARAM>(L"输入消息，回车发送"));
        SendMessageW(group_name_input, EM_SETCUEBANNER, FALSE,
                     reinterpret_cast<LPARAM>(L"群名称"));
        apply_fonts();
        switch_view(ViewPage::chats);
        rebuild_conversation_list();
        update_conversation_header();
        update_controls();
        open_event_log();
        append_activity(L"填写昵称，点击“连接服务器”进入聊天室。");
        SetFocus(name_input);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* min_max = reinterpret_cast<MINMAXINFO*>(lparam);
        RECT minimum{0, 0, scale(800), scale(560)};
        AdjustWindowRectEx(&minimum, WS_OVERLAPPEDWINDOW, FALSE, 0);
        min_max->ptMinTrackSize.x = minimum.right - minimum.left;
        min_max->ptMinTrackSize.y = minimum.bottom - minimum.top;
        return 0;
    }
    case WM_DPICHANGED: {
        current_dpi = LOWORD(wparam);
        const auto* suggested = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        rebuild_fonts();
        apply_fonts();
        RECT client{};
        GetClientRect(window, &client);
        layout_controls(client.right - client.left, client.bottom - client.top);
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    }
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED && chat_log != nullptr && activity_log != nullptr) {
            layout_controls(LOWORD(lparam), HIWORD(lparam));
            InvalidateRect(window, nullptr, TRUE);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint_window(window);
        return 0;
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
        if (item->CtlType == ODT_BUTTON) {
            draw_button(*item);
            return TRUE;
        }
        if (item->CtlType == ODT_LISTBOX) {
            draw_list_item(*item);
            return TRUE;
        }
        break;
    }
    case WM_MEASUREITEM: {
        auto* item = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
        if (item->CtlType == ODT_LISTBOX) {
            item->itemHeight = scale(32);
            return TRUE;
        }
        break;
    }
    case WM_TIMER:
        if (wparam == ID_DISCONNECT_TIMER) {
            KillTimer(window, ID_DISCONNECT_TIMER);
            if (server_socket != INVALID_SOCKET) {
                append_activity(L"关闭服务器连接");
                shutdown(server_socket, SD_BOTH);
            }
            return 0;
        }
        if (wparam == ID_DEMO_TIMER && demo_enabled) {
            run_demo_step();
            return 0;
        }
        break;
    case WM_COMMAND: {
        const int identifier = LOWORD(wparam);
        const int notification = HIWORD(wparam);
        if (identifier == ID_CONVERSATIONS && notification == LBN_SELCHANGE) {
            const LRESULT selected = SendMessageW(conversation_list, LB_GETCURSEL, 0, 0);
            if (selected != LB_ERR && static_cast<std::size_t>(selected) < conversation_entries.size()) {
                const ConversationEntry& entry = conversation_entries[static_cast<std::size_t>(selected)];
                set_active_conversation(entry.kind, entry.key);
            }
            return 0;
        }
        if (identifier == ID_GROUP_LIST && notification == LBN_SELCHANGE) {
            const LRESULT selected = SendMessageW(group_list, LB_GETCURSEL, 0, 0);
            if (selected != LB_ERR) {
                const LRESULT index = SendMessageW(group_list, LB_GETITEMDATA,
                                                   static_cast<WPARAM>(selected), 0);
                if (index != LB_ERR && static_cast<std::size_t>(index) < group_names.size()) {
                    set_active_conversation(
                        ConversationKind::group_chat,
                        group_key(from_utf8(group_names[static_cast<std::size_t>(index)])));
                }
            }
            return 0;
        }
        if (identifier == ID_ONLINE_USERS && notification == LBN_SELCHANGE &&
            active_view == ViewPage::chats) {
            const LRESULT selected = SendMessageW(online_user_list, LB_GETCURSEL, 0, 0);
            if (selected != LB_ERR) {
                const LRESULT index = SendMessageW(online_user_list, LB_GETITEMDATA,
                                                   static_cast<WPARAM>(selected), 0);
                if (index != LB_ERR && static_cast<std::size_t>(index) < online_user_entries.size()) {
                    const std::string& user = online_user_entries[static_cast<std::size_t>(index)];
                    if (user != current_user) {
                        remember_direct_contact(user);
                        set_active_conversation(ConversationKind::direct_chat,
                                                direct_key(from_utf8(user)));
                    }
                }
            }
            return 0;
        }
        if (notification == BN_CLICKED) {
            switch (identifier) {
            case ID_CONNECT:
                connect_to_server();
                return 0;
            case ID_REFRESH:
                refresh_remote_lists();
                return 0;
            case ID_QUIT:
                quit_chat();
                return 0;
            case ID_NAV_CHAT:
                switch_view(ViewPage::chats);
                return 0;
            case ID_NAV_GROUP:
                switch_view(ViewPage::groups);
                return 0;
            case ID_CREATE_GROUP:
                send_group_action("CREATE");
                return 0;
            case ID_ADD_GROUP_MEMBER:
                add_selected_group_members();
                return 0;
            case ID_LEAVE_GROUP:
                send_group_action("LEAVE");
                return 0;
            case ID_SEND:
                send_message();
                return 0;
            }
        }
        break;
    }
    case WM_CHAT_FRAME: {
        std::unique_ptr<std::string> frame(reinterpret_cast<std::string*>(lparam));
        display_protocol_frame(*frame);
        return 0;
    }
    case WM_CHAT_DISCONNECTED:
        KillTimer(window, ID_DISCONNECT_TIMER);
        disconnecting = false;
        if (receive_thread != nullptr) {
            WaitForSingleObject(receive_thread, INFINITE);
            CloseHandle(receive_thread);
            receive_thread = nullptr;
        }
        if (server_socket != INVALID_SOCKET) {
            closesocket(server_socket);
            server_socket = INVALID_SOCKET;
        }
        if (connected.exchange(false)) {
            joined = false;
            online_users.clear();
            group_names.clear();
            rebuild_online_user_list();
            rebuild_group_list();
            append_activity(L"服务器连接已断开");
            update_controls();
        }
        if (close_pending) {
            DestroyWindow(window);
        }
        return 0;
    case WM_CLOSE:
        if (connected.load()) {
            close_pending = true;
            quit_chat();
        } else {
            DestroyWindow(window);
        }
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC device = reinterpret_cast<HDC>(wparam);
        HWND control = reinterpret_cast<HWND>(lparam);
        SetBkMode(device, TRANSPARENT);
        if (control == brand_label) {
            SetTextColor(device, RGB(255, 255, 255));
            return reinterpret_cast<INT_PTR>(header_brush);
        }
        if (control == activity_log) {
            SetBkMode(device, OPAQUE);
            SetBkColor(device, COLOR_ACTIVITY);
            SetTextColor(device, COLOR_MUTED);
            return reinterpret_cast<INT_PTR>(activity_brush);
        }
        SetBkColor(device, COLOR_CARD);
        if (control == status_label) {
            SetTextColor(device, COLOR_MUTED);
        } else {
            SetTextColor(device, COLOR_TEXT);
        }
        return reinterpret_cast<INT_PTR>(card_brush);
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC device = reinterpret_cast<HDC>(wparam);
        HWND control = reinterpret_cast<HWND>(lparam);
        if (control == activity_log) {
            SetBkColor(device, COLOR_ACTIVITY);
            SetTextColor(device, COLOR_MUTED);
            return reinterpret_cast<INT_PTR>(activity_brush);
        }
        SetBkColor(device, COLOR_CARD);
        SetTextColor(device, COLOR_TEXT);
        return reinterpret_cast<INT_PTR>(card_brush);
    }
    case WM_DESTROY:
        if (server_socket != INVALID_SOCKET) {
            shutdown(server_socket, SD_BOTH);
        }
        if (receive_thread != nullptr) {
            WaitForSingleObject(receive_thread, INFINITE);
            CloseHandle(receive_thread);
            receive_thread = nullptr;
        }
        if (server_socket != INVALID_SOCKET) {
            closesocket(server_socket);
            server_socket = INVALID_SOCKET;
        }
        DeleteObject(regular_font);
        DeleteObject(medium_font);
        DeleteObject(title_font);
        DeleteObject(small_font);
        DeleteObject(canvas_brush);
        DeleteObject(card_brush);
        DeleteObject(header_brush);
        DeleteObject(activity_brush);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(window, message, wparam, lparam);
}

}

// Win32 程序入口。
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line, int show_command) {
    enable_dpi_awareness();
    current_dpi = get_system_dpi();

    std::wistringstream arguments(command_line);
    std::wstring flag;
    std::wstring demo_port;
    if (arguments >> flag) {
        if (flag == L"--demo" && arguments >> demo_role >> demo_port) {
            demo_enabled = true;
            requested_name = demo_role;
        } else if (flag == L"--name" && arguments >> requested_name) {
            demo_enabled = false;
        }
    }

    if (!init_sockets()) {
        MessageBoxW(nullptr, L"WSAStartup() 失败。", L"网络聊天室", MB_ICONERROR);
        return 2;
    }

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.hInstance = instance;
    window_class.lpfnWndProc = window_procedure;
    window_class.lpszClassName = L"NChatWin32Window";
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    window_class.hbrBackground = nullptr;

    if (!RegisterClassExW(&window_class)) {
        cleanup_sockets();
        return 2;
    }

    MONITORINFO monitor{sizeof(MONITORINFO)};
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor);
    RECT frame{0, 0, scale(1040), scale(700)};
    AdjustWindowRectEx(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0);
    const int window_width = std::min(static_cast<int>(frame.right - frame.left),
                                     static_cast<int>(monitor.rcWork.right - monitor.rcWork.left) - scale(24));
    const int window_height = std::min(static_cast<int>(frame.bottom - frame.top),
                                      static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top) - scale(24));
    const int window_x = monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - window_width) / 2;
    const int window_y = monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - window_height) / 2;
    HWND window = CreateWindowExW(
        0, window_class.lpszClassName, L"网络聊天室",
        WS_OVERLAPPEDWINDOW,
        window_x, window_y, window_width, window_height,
        nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
        cleanup_sockets();
        return 2;
    }

    ShowWindow(window, show_command);
    UpdateWindow(window);
    if (demo_enabled) {
        SetWindowTextW(ip_input, L"127.0.0.1");
        SetWindowTextW(port_input, demo_port.c_str());
        SetWindowTextW(name_input, demo_role.c_str());
        SetTimer(window, ID_DEMO_TIMER, 1200, nullptr);
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.hwnd == message_input && message.message == WM_KEYDOWN &&
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

int main() {
    return 1;
}

#endif
