// 检查原生窗口布局和消息分区。
#include "../src/gui_client.cpp"
#include <functional>
#include <iostream>

namespace {
int passed = 0;
int failed = 0;

// 记录断言结果。
void check(bool result, const char* label) {
    std::cout << (result ? "[PASS] " : "[FAIL] ") << label << '\n';
    result ? ++passed : ++failed;
}

// 读取控件在客户区内的位置。
RECT control_rect(HWND control) {
    RECT rect{};
    GetWindowRect(control, &rect);
    MapWindowPoints(nullptr, main_window, reinterpret_cast<POINT*>(&rect), 2);
    return rect;
}

// 等待接收线程传回窗口消息。
bool wait_for(const std::function<bool()>& ready) {
    const ULONGLONG until = GetTickCount64() + 4000;
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (ready()) {
            return true;
        }
        Sleep(10);
    } while (GetTickCount64() < until);
    return false;
}

// 查询显示文本。
bool contains(HWND control, const wchar_t* text) {
    return get_control_text(control).find(text) != std::wstring::npos;
}

// 检查尺寸、分区顺序和控件交叠。
void test_layouts() {
    int broken = 0;
    int cases = 0;
    const SIZE sizes[] = {{800, 560}, {900, 600}, {999, 640}, {1000, 640},
                          {1040, 700}, {1400, 900}};
    for (UINT dpi : {96U, 120U, 144U, 192U}) {
        current_dpi = dpi;
        rebuild_fonts();
        apply_fonts();
        for (SIZE size : sizes) {
            for (ViewPage page : {ViewPage::chats, ViewPage::groups}) {
                switch_view(page);
                const int width = scale(size.cx);
                const int height = scale(size.cy);
                RECT frame{0, 0, width, height};
                AdjustWindowRectEx(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0);
                SetWindowPos(main_window, nullptr, 0, 0, frame.right - frame.left,
                             frame.bottom - frame.top, SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER);
                layout_controls(width, height);
                const RECT chat = control_rect(chat_log);
                const RECT input = control_rect(message_input);
                const RECT send = control_rect(send_button);
                const RECT activity = control_rect(activity_log);
                const RECT status = control_rect(status_label);
                const RECT side = control_rect(online_user_list);
                bool ok = chat.bottom <= input.top && input.top - chat.bottom <= scale(12) &&
                          input.bottom == send.bottom && input.top == send.top &&
                          input.bottom + scale(12) <= status.top &&
                          side.bottom < status.top && status.bottom < activity.top &&
                          activity.left < chat.left && activity.right == chat.right &&
                          activity.bottom - activity.top >= scale(90) &&
                          chat.bottom - chat.top >= scale(80);
                const HWND controls[] = {brand_label, nav_chat_button, nav_group_button,
                    server_label, ip_input, port_label, port_input, name_label, name_input,
                    connect_button, quit_button, refresh_button, sidebar_title, conversation_list,
                    group_list, online_title, online_user_list, conversation_title, group_name_input,
                    create_group_button, join_group_button, leave_group_button, chat_log,
                    message_input, send_button, activity_title, status_label, activity_log};
                for (std::size_t i = 0; i < std::size(controls); ++i) {
                    if ((GetWindowLongPtrW(controls[i], GWL_STYLE) & WS_VISIBLE) == 0) {
                        continue;
                    }
                    const RECT a = control_rect(controls[i]);
                    ok &= a.left >= 0 && a.top >= 0 && a.right <= width && a.bottom <= height &&
                          a.left < a.right && a.top < a.bottom;
                    for (std::size_t j = i + 1; j < std::size(controls); ++j) {
                        if ((GetWindowLongPtrW(controls[j], GWL_STYLE) & WS_VISIBLE) == 0) {
                            continue;
                        }
                        RECT overlap{};
                        const RECT b = control_rect(controls[j]);
                        ok &= !IntersectRect(&overlap, &a, &b);
                    }
                }
                if (!ok) {
                    ++broken;
                }
                ++cases;
            }
        }
    }
    std::cout << "LAYOUT_CASES=" << cases << " FAIL=" << broken << '\n';
    check(broken == 0, "48 layouts: composer under messages; full-width activity below; no overlap");
    current_dpi = get_window_dpi(main_window);
    rebuild_fonts();
    apply_fonts();
    switch_view(ViewPage::chats);
}

// 检查协议帧在两个文本区中的归属。
void test_routing() {
    current_user = "Alice";
    histories.clear();
    activity_lines.clear();
    set_active_conversation(ConversationKind::public_room, L"public");
    display_protocol_frame("MSG|2026-10-21 12:00:00|Bob|1|PUBLIC_MARK");
    display_protocol_frame("NOTICE|2026-10-21 12:00:01|Bob joined the room");
    check(contains(chat_log, L"PUBLIC_MARK") && !contains(chat_log, L"进入") &&
          contains(activity_log, L"Bob 进入公共聊天室") && !contains(activity_log, L"PUBLIC_MARK"),
          "public messages and room notices use separate logs");
    set_active_conversation(ConversationKind::direct_chat, L"direct:Bob");
    check(get_control_text(chat_log).empty(), "new private conversation starts empty");
    display_protocol_frame("PM|2026-10-21 12:00:02|Bob|Alice|2|PRIVATE_MARK");
    check(contains(chat_log, L"PRIVATE_MARK") && !contains(chat_log, L"PUBLIC_MARK"),
          "private history stays separate from public history");
    switch_view(ViewPage::groups);
    set_active_conversation(ConversationKind::group_chat, L"group:Study");
    display_protocol_frame("GMSG|2026-10-21 12:00:03|Study|Bob|3|GROUP_MARK");
    display_protocol_frame("GNOTICE|2026-10-21 12:00:04|Study|Bob joined the group");
    display_protocol_frame("ERR|TEST|ERROR_MARK");
    check(contains(chat_log, L"GROUP_MARK") && !contains(chat_log, L"进入") &&
          !contains(chat_log, L"ERROR_MARK") && contains(activity_log, L"Bob 进入群聊 Study") &&
          contains(activity_log, L"ERROR_MARK"), "group notices and errors stay in the activity log");
    switch_view(ViewPage::chats);
    check(contains(activity_log, L"进入群聊") && contains(chat_log, L"PUBLIC_MARK"),
          "activity remains visible when changing conversations");
    connected.store(false);
    joined = false;
    update_controls();
    check(IsWindowEnabled(connect_button) && !IsWindowEnabled(quit_button) &&
          !IsWindowEnabled(send_button), "offline connect/disconnect/send states");
}

// 通过真实服务器检查连接、发消息和断开。
void test_live(unsigned short port) {
    open_event_log();
    append_activity(L"GUI 真实 TCP 测试开始");
    SetWindowTextW(port_input, std::to_wstring(port).c_str());
    SetWindowTextW(name_input, L"UiAlice");
    connect_to_server();
    check(wait_for([] { return joined && online_users.size() == 1; }), "live GUI login and user list");
    check(!IsWindowEnabled(connect_button) && IsWindowEnabled(quit_button) &&
          IsWindowEnabled(send_button), "online connect/disconnect/send states");
    SOCKET peer = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (connect(peer, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        check(false, "live peer connect");
        closesocket(peer);
        return;
    }
    chat::send_frame(peer, "JOIN|UiBob");
    check(wait_for([] { return online_users.size() == 2 && contains(activity_log, L"UiBob 进入公共聊天室"); }),
          "live second user enters activity log");
    SetWindowTextW(message_input, L"你好，hello");
    send_message();
    check(wait_for([] { return contains(chat_log, L"你好，hello"); }) &&
          !contains(chat_log, L"进入") && !contains(activity_log, L"你好，hello"),
          "live UTF-8 chat echo stays in message area");
    check(messages_sent == 1 && messages_confirmed == 1 && awaiting_echo.empty(),
          "live sent message matched its own server echo");
    chat::send_frame(peer, "PM|1|UiAlice|live-private");
    check(wait_for([] { return histories.count(L"direct:UiBob") != 0; }), "live private message received");
    set_active_conversation(ConversationKind::direct_chat, L"direct:UiBob");
    check(contains(chat_log, L"live-private") && !contains(chat_log, L"你好，hello"), "live private view isolated");
    chat::send_frame(peer, "QUIT");
    check(wait_for([] { return online_users.size() == 1 && contains(activity_log, L"UiBob 离开公共聊天室"); }),
          "live user leave recorded at bottom");
    closesocket(peer);
    SOCKET abrupt = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (connect(abrupt, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
        chat::send_frame(abrupt, "JOIN|UiCrash");
        check(wait_for([] { return online_users.size() == 2; }), "live abrupt-disconnect peer joined");
        shutdown(abrupt, SD_BOTH);
        closesocket(abrupt);
        check(wait_for([] { return online_users.size() == 1 && contains(activity_log, L"UiCrash 离开公共聊天室"); }),
              "live abrupt disconnect removes online user");
    } else {
        closesocket(abrupt);
        check(false, "live abrupt-disconnect peer joined");
    }
    quit_chat();
    check(wait_for([] { return !connected.load(); }) && IsWindowEnabled(connect_button) &&
          !IsWindowEnabled(quit_button), "live disconnect restores connect action");
    connect_to_server();
    check(wait_for([] { return joined; }), "live reconnect");
    SendMessageW(main_window, WM_CLOSE, 0, 0);
    check(wait_for([] { return !IsWindow(main_window) && !connected.load(); }),
          "live titlebar close sends QUIT and closes window");
    std::ifstream log(event_log_path, std::ios::binary);
    const std::string saved((std::istreambuf_iterator<char>(log)), {});
    check(saved.find("QUIT") != std::string::npos && saved.find("MSG") != std::string::npos,
          "live session events persisted to UTF-8 log");
}
}

// 创建隐藏测试窗口并执行检查。
int main(int argc, char** argv) {
    enable_dpi_awareness();
    current_dpi = get_system_dpi();
    if (!init_sockets()) {
        return 2;
    }
    WNDCLASSW cls{};
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpfnWndProc = window_procedure;
    cls.lpszClassName = L"ChatLayoutTest";
    RegisterClassW(&cls);
    HWND window = CreateWindowExW(0, cls.lpszClassName, L"Layout test", WS_OVERLAPPEDWINDOW,
                                  0, 0, scale(1040), scale(700), nullptr, nullptr, cls.hInstance, nullptr);
    if (window == nullptr) {
        return 2;
    }
    event_log_path.clear();
    test_layouts();
    test_routing();
    if (argc == 2) {
        test_live(static_cast<unsigned short>(std::stoi(argv[1])));
    }
    if (IsWindow(window)) {
        DestroyWindow(window);
    }
    cleanup_sockets();
    std::cout << "GUI_TESTS PASS=" << passed << " FAIL=" << failed << '\n';
    return failed == 0 ? 0 : 1;
}
