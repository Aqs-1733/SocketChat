#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <pcap.h>

#include "capture_service.h"
#include "npcap_runtime.h"

#include <atomic>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <new>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr UINT WM_PACKET = WM_APP + 1;
constexpr UINT WM_STATUS = WM_APP + 2;
constexpr UINT WM_FINISHED = WM_APP + 3;
constexpr UINT_PTR REFRESH_TIMER = 1;
constexpr int MAX_VISIBLE_ROWS = 1500;
constexpr int MAX_PENDING_EVENTS = 400;

constexpr int ID_DEVICE = 101;
constexpr int ID_REFRESH = 102;
constexpr int ID_FILTER = 103;
constexpr int ID_COUNT = 104;
constexpr int ID_START = 105;
constexpr int ID_STOP = 106;
constexpr int ID_PACKETS = 107;

struct CaptureOutcome {
    bool success = false;
    netlab::CaptureStats stats;
    std::string error;
};

struct Application {
    HWND window = nullptr;
    HWND title = nullptr;
    HWND device_label = nullptr;
    HWND device_combo = nullptr;
    HWND refresh_button = nullptr;
    HWND filter_label = nullptr;
    HWND filter_input = nullptr;
    HWND count_label = nullptr;
    HWND count_input = nullptr;
    HWND start_button = nullptr;
    HWND stop_button = nullptr;
    HWND status_label = nullptr;
    HWND packet_table = nullptr;
    HWND detail_label = nullptr;
    HWND details = nullptr;
    HFONT font = nullptr;
    HFONT title_font = nullptr;
    std::vector<netlab::DeviceInfo> devices;
    std::vector<netlab::PacketRecord> records;
    std::thread worker;
    std::atomic<bool> running{false};
    std::atomic<std::uint64_t> total_received{0};
    std::atomic<std::uint64_t> total_invalid{0};
    std::atomic<std::uint64_t> skipped_on_screen{0};
    std::atomic<int> pending_events{0};
    bool active = false;
    bool network_initialized = false;
    std::wstring last_status = L"请选择网卡，点击“开始捕获”。";
};

Application app;

// 将 Npcap 文本转换为 Win32 宽字符。
std::wstring widen(const std::string& input) {
    if (input.empty()) {
        return {};
    }

    UINT codepage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int size = MultiByteToWideChar(codepage, flags, input.data(),
                                   static_cast<int>(input.size()), nullptr, 0);
    if (size == 0) {
        codepage = CP_ACP;
        flags = 0;
        size = MultiByteToWideChar(codepage, flags, input.data(),
                                   static_cast<int>(input.size()), nullptr, 0);
    }
    if (size == 0) {
        return L"(Invalid text)";
    }

    std::wstring output(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(codepage, flags, input.data(),
                        static_cast<int>(input.size()), output.data(), size);
    return output;
}

std::string to_utf8(const std::wstring& input) {
    if (input.empty()) {
        return {};
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, input.data(),
                                         static_cast<int>(input.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }

    std::string output(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, input.data(),
                        static_cast<int>(input.size()),
                        output.data(), size, nullptr, nullptr);
    return output;
}

std::wstring get_text(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring value(static_cast<std::size_t>(length + 1), L'\0');
    GetWindowTextW(control, value.data(), length + 1);
    value.resize(static_cast<std::size_t>(length));
    return value;
}

bool parse_count(const std::wstring& text, std::uint64_t& count) {
    if (text.empty()) {
        return false;
    }

    count = 0;
    for (const wchar_t c : text) {
        if (c < L'0' || c > L'9') {
            return false;
        }
        const auto digit = static_cast<std::uint64_t>(c - L'0');
        if (count > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            return false;
        }
        count = count * 10U + digit;
    }
    return true;
}

std::wstring hex_value(unsigned value, int digits) {
    std::wostringstream output;
    output << L"0x" << std::uppercase << std::hex << std::setw(digits)
           << std::setfill(L'0') << value;
    return output.str();
}

HWND make_control(const wchar_t* class_name, const wchar_t* text, DWORD style,
                  DWORD extended_style, int id) {
    HWND control = CreateWindowExW(
        extended_style, class_name, text, WS_CHILD | WS_VISIBLE | style,
        0, 0, 100, 24, app.window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);

    if (control != nullptr) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(app.font), TRUE);
    }
    return control;
}

void place(HWND control, int x, int y, int width, int height) {
    if (control != nullptr) {
        MoveWindow(control, x, y, width, height, TRUE);
    }
}

// 根据窗口大小调整控件位置。
void resize_controls(int width, int height) {
    if (width < 820 || height < 520) {
        return;
    }

    place(app.title, 20, 12, width - 40, 42);
    place(app.device_label, 20, 68, 80, 28);
    place(app.device_combo, 105, 65, width - 260, 400);
    place(app.refresh_button, width - 140, 65, 120, 29);

    place(app.filter_label, 20, 107, 80, 28);
    place(app.filter_input, 105, 104, width - 125, 28);

    place(app.count_label, 20, 146, 80, 28);
    place(app.count_input, 105, 143, 112, 28);
    place(app.start_button, width - 285, 142, 125, 31);
    place(app.stop_button, width - 150, 142, 130, 31);
    place(app.status_label, 20, 186, width - 40, 30);

    const int table_height = (height - 410 > 120) ? height - 410 : 120;
    place(app.packet_table, 20, 223, width - 40, table_height);
    const int detail_top = 235 + table_height;
    place(app.detail_label, 20, detail_top, width - 40, 26);
    place(app.details, 20, detail_top + 30, width - 40,
          height - detail_top - 46);
}

void set_status(const std::wstring& status) {
    app.last_status = status;
    SetWindowTextW(app.status_label, status.c_str());
}

// 从 Npcap 刷新网卡下拉列表。
void refresh_devices() {
    if (app.active) {
        return;
    }

    std::string error;
    std::vector<netlab::DeviceInfo> devices;
    if (!netlab::enumerate_devices(devices, error)) {
        MessageBoxW(app.window, widen(error).c_str(), L"网卡列表获取失败", MB_ICONERROR);
        return;
    }

    app.devices = std::move(devices);
    SendMessageW(app.device_combo, CB_RESETCONTENT, 0, 0);

    for (const auto& device : app.devices) {
        std::wstring name = widen(device.description);
        if (device.loopback) {
            name += L" [Loopback]";
        }
        if (!device.ipv4_addresses.empty()) {
            name += L"  (" + widen(device.ipv4_addresses.front()) + L")";
        }
        SendMessageW(app.device_combo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(name.c_str()));
    }

    if (!app.devices.empty()) {
        SendMessageW(app.device_combo, CB_SETCURSEL, 0, 0);
        set_status(L"网卡列表已更新，共 " + std::to_wstring(app.devices.size()) + L" 个。" );
    } else {
        set_status(L"未找到网卡，请确认 Npcap 已安装。" );
    }
}

// 在列表中显示一帧并限制可见行数。
void append_packet(netlab::PacketRecord record) {
    if (static_cast<int>(app.records.size()) == MAX_VISIBLE_ROWS) {
        ListView_DeleteItem(app.packet_table, 0);
        app.records.erase(app.records.begin());
    }

    const int row = static_cast<int>(app.records.size());
    app.records.push_back(std::move(record));
    const auto& current = app.records.back();

    std::wstring sequence = std::to_wstring(current.sequence);
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = row;
    item.pszText = sequence.data();
    ListView_InsertItem(app.packet_table, &item);

    const std::wstring time = widen(current.local_time);
    const bool valid = current.status == netlab::ParseStatus::Ok;
    const std::wstring source = valid ? widen(netlab::format_mac(current.frame.source)) : L"-";
    const std::wstring destination = valid ? widen(netlab::format_mac(current.frame.destination)) : L"-";
    const std::wstring protocol = valid ?
        hex_value(current.frame.type_or_length, 4) + L" " +
        widen(netlab::describe_protocol(current.frame.type_or_length)) : L"无效数据";
    const std::wstring first = valid && current.frame.has_first_byte ?
        hex_value(current.frame.first_byte, 2) : L"N/A";
    const std::wstring length = std::to_wstring(current.captured_length);

    const std::wstring fields[] = {time, source, destination, protocol, first, length};
    for (int column = 0; column < 6; ++column) {
        ListView_SetItemText(app.packet_table, row, column + 1,
                             const_cast<wchar_t*>(fields[column].c_str()));
    }

    if (row == 0) {
        ListView_SetItemState(app.packet_table, row, LVIS_SELECTED, LVIS_SELECTED);
    }
}

// 显示当前选中数据帧的字段。
void show_details() {
    const int index = ListView_GetNextItem(app.packet_table, -1, LVNI_SELECTED);
    if (index < 0 || static_cast<std::size_t>(index) >= app.records.size()) {
        SetWindowTextW(app.details, L"选择上方数据帧查看字段。");
        return;
    }

    const auto& record = app.records[static_cast<std::size_t>(index)];
    std::wstring text = L"序号：" + std::to_wstring(record.sequence) +
                        L"    时间：" + widen(record.local_time) + L"\r\n" +
                        L"捕获长度：" + std::to_wstring(record.captured_length) +
                        L" 字节    原始长度：" + std::to_wstring(record.wire_length) + L" 字节\r\n";

    if (record.status != netlab::ParseStatus::Ok) {
        text += L"解析失败：" + widen(netlab::describe_status(record.status));
        SetWindowTextW(app.details, text.c_str());
        return;
    }

    const auto& frame = record.frame;
    text += L"源 MAC：" + widen(netlab::format_mac(frame.source)) +
            L"    目的 MAC：" + widen(netlab::format_mac(frame.destination)) + L"\r\n" +
            L"外层类型/长度：" + hex_value(frame.type_or_length, 4) +
            L"    字段含义：" + widen(netlab::describe_protocol(frame.type_or_length)) + L"\r\n";

    text += L"类型字段之后第一个字节：";
    text += frame.has_first_byte ?
        hex_value(frame.first_byte, 2) + L" (" + std::to_wstring(frame.first_byte) + L")" :
        L"N/A";

    for (std::size_t i = 0; i < frame.vlans.size(); ++i) {
        const auto& tag = frame.vlans[i];
        text += L"\r\nVLAN " + std::to_wstring(i + 1) +
                L"：TPID=" + hex_value(tag.tpid, 4) +
                L"  ID=" + std::to_wstring(tag.id()) +
                L"  PCP=" + std::to_wstring(tag.priority()) +
                L"  DEI=" + std::to_wstring(tag.drop_eligible() ? 1 : 0);
    }

    if (!frame.vlans.empty()) {
        text += L"\r\n内层类型：" + hex_value(frame.protocol_field, 4) +
                L"    数据偏移：" + std::to_wstring(frame.data_offset) +
                L"    数据首字节：";
        text += frame.has_payload_first_byte ? hex_value(frame.payload_first_byte, 2) : L"N/A";
    }

    if (record.captured_length < record.wire_length) {
        text += L"\r\n注意：数据帧被截断。";
    }

    SetWindowTextW(app.details, text.c_str());
}

// 启动抓包线程并锁定输入控件。
void start_capture() {
    if (app.active) {
        return;
    }

    const LRESULT selected = SendMessageW(app.device_combo, CB_GETCURSEL, 0, 0);
    if (selected == CB_ERR || selected < 0 ||
        static_cast<std::size_t>(selected) >= app.devices.size()) {
        MessageBoxW(app.window, L"请先选择有效网卡。", L"提示", MB_ICONWARNING);
        return;
    }

    std::uint64_t count = 0;
    if (!parse_count(get_text(app.count_input), count)) {
        MessageBoxW(app.window, L"抓包数量请输入非负整数，0 表示不限。",
                    L"输入错误", MB_ICONWARNING);
        return;
    }

    netlab::CaptureOptions options;
    options.device_name = app.devices[static_cast<std::size_t>(selected)].name;
    options.filter = to_utf8(get_text(app.filter_input));
    options.count = count;

    app.records.clear();
    ListView_DeleteAllItems(app.packet_table);
    SetWindowTextW(app.details, L"选择上方数据帧查看字段。");
    app.total_received.store(0);
    app.total_invalid.store(0);
    app.skipped_on_screen.store(0);
    app.pending_events.store(0);
    app.running.store(true);
    app.active = true;

    EnableWindow(app.device_combo, FALSE);
    EnableWindow(app.refresh_button, FALSE);
    EnableWindow(app.filter_input, FALSE);
    EnableWindow(app.count_input, FALSE);
    EnableWindow(app.start_button, FALSE);
    EnableWindow(app.stop_button, TRUE);
    set_status(L"正在捕获数据帧……");

    // 抓包工作在线程中运行，界面只接收消息。
    const HWND window = app.window;
    app.worker = std::thread([window, options]() {
        netlab::CaptureCallbacks callbacks;
        callbacks.on_message = [window](const std::string& message) {
            auto* text = new (std::nothrow) std::string(message);
            if (text != nullptr && !PostMessageW(window, WM_STATUS, 0,
                                                  reinterpret_cast<LPARAM>(text))) {
                delete text;
            }
        };
        callbacks.on_packet = [window](const netlab::PacketRecord& record) {
            app.total_received.fetch_add(1);
            if (record.status != netlab::ParseStatus::Ok) {
                app.total_invalid.fetch_add(1);
            }

            // 限制界面消息积压，抓包本身不丢弃。
            if (app.pending_events.fetch_add(1) >= MAX_PENDING_EVENTS) {
                app.pending_events.fetch_sub(1);
                app.skipped_on_screen.fetch_add(1);
                return;
            }

            auto* item = new (std::nothrow) netlab::PacketRecord(record);
            if (item == nullptr ||
                !PostMessageW(window, WM_PACKET, 0, reinterpret_cast<LPARAM>(item))) {
                delete item;
                app.pending_events.fetch_sub(1);
                app.skipped_on_screen.fetch_add(1);
            }
        };

        auto* outcome = new (std::nothrow) CaptureOutcome;
        if (outcome == nullptr) {
            PostMessageW(window, WM_FINISHED, 0, 0);
            return;
        }

        outcome->success = netlab::capture_live(options, callbacks, app.running,
                                                 outcome->stats, outcome->error);
        if (!PostMessageW(window, WM_FINISHED, 0,
                          reinterpret_cast<LPARAM>(outcome))) {
            delete outcome;
        }
    });
}

// 回收线程并恢复控件状态。
void finish_capture(const CaptureOutcome* outcome) {
    if (app.worker.joinable()) {
        app.worker.join();
    }

    app.running.store(false);
    app.active = false;
    EnableWindow(app.device_combo, TRUE);
    EnableWindow(app.refresh_button, TRUE);
    EnableWindow(app.filter_input, TRUE);
    EnableWindow(app.count_input, TRUE);
    EnableWindow(app.start_button, TRUE);
    EnableWindow(app.stop_button, FALSE);

    std::wstring message = L"已停止。总捕获：" +
                           std::to_wstring(app.total_received.load()) +
                           L"，解析失败：" +
                           std::to_wstring(app.total_invalid.load()) +
                           L"，界面限流：" +
                           std::to_wstring(app.skipped_on_screen.load());
    if (outcome != nullptr && outcome->stats.has_driver_stats) {
        message += L"，驱动丢包：" + std::to_wstring(outcome->stats.ps_drop);
    }
    set_status(message);

    if (outcome != nullptr && !outcome->success) {
        MessageBoxW(app.window, widen(outcome->error).c_str(),
                    L"捕获失败", MB_ICONERROR);
    }
}

void update_running_status() {
    if (!app.active) {
        return;
    }

    const std::wstring status = L"正在捕获 | 已抓取 " +
        std::to_wstring(app.total_received.load()) +
        L" 帧 | 解析失败 " + std::to_wstring(app.total_invalid.load()) +
        L" 帧 | 屏幕限流 " + std::to_wstring(app.skipped_on_screen.load()) + L" 帧";
    SetWindowTextW(app.status_label, status.c_str());
}

void create_controls() {
    app.font = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    app.title_font = CreateFontW(-27, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");

    app.title = make_control(L"STATIC", L"计算机网络实验二 · 以太网数据包捕获",
                             SS_LEFT, 0, 0);
    SendMessageW(app.title, WM_SETFONT, reinterpret_cast<WPARAM>(app.title_font), TRUE);

    app.device_label = make_control(L"STATIC", L"网卡", SS_LEFT, 0, 0);
    app.device_combo = make_control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
                                    0, ID_DEVICE);
    app.refresh_button = make_control(L"BUTTON", L"刷新网卡", BS_PUSHBUTTON, 0, ID_REFRESH);
    app.filter_label = make_control(L"STATIC", L"过滤条件", SS_LEFT, 0, 0);
    app.filter_input = make_control(L"EDIT", L"", ES_AUTOHSCROLL,
                                    WS_EX_CLIENTEDGE, ID_FILTER);
    app.count_label = make_control(L"STATIC", L"抓包数量", SS_LEFT, 0, 0);
    app.count_input = make_control(L"EDIT", L"0", ES_AUTOHSCROLL,
                                   WS_EX_CLIENTEDGE, ID_COUNT);
    app.start_button = make_control(L"BUTTON", L"开始捕获", BS_DEFPUSHBUTTON, 0, ID_START);
    app.stop_button = make_control(L"BUTTON", L"停止捕获", BS_PUSHBUTTON, 0, ID_STOP);
    EnableWindow(app.stop_button, FALSE);
    app.status_label = make_control(L"STATIC", L"请选择网卡，点击开始捕获。", SS_LEFT, 0, 0);

    app.packet_table = make_control(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL |
                                    LVS_SHOWSELALWAYS, WS_EX_CLIENTEDGE, ID_PACKETS);
    ListView_SetExtendedListViewStyle(app.packet_table, LVS_EX_FULLROWSELECT |
                                     LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    const wchar_t* names[] = {L"序号", L"捕获时间", L"源 MAC", L"目的 MAC",
                               L"类型/长度", L"类型后首字节", L"长度"};
    const int widths[] = {66, 178, 173, 173, 180, 139, 75};
    for (int i = 0; i < 7; ++i) {
        LVCOLUMNW column{};
        column.mask = LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;
        column.cx = widths[i];
        column.pszText = const_cast<wchar_t*>(names[i]);
        column.iSubItem = i;
        ListView_InsertColumn(app.packet_table, i, &column);
    }

    app.detail_label = make_control(L"STATIC", L"数据帧详情", SS_LEFT, 0, 0);
    app.details = make_control(L"EDIT", L"选择上方数据帧查看字段。",
                               ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL |
                               WS_VSCROLL, WS_EX_CLIENTEDGE, 0);
}

// 关闭窗口前释放尚未处理的消息数据。
void discard_pending_messages() {
    MSG message{};
    while (PeekMessageW(&message, app.window, WM_PACKET, WM_FINISHED, PM_REMOVE)) {
        if (message.message == WM_PACKET) {
            delete reinterpret_cast<netlab::PacketRecord*>(message.lParam);
        } else if (message.message == WM_STATUS) {
            delete reinterpret_cast<std::string*>(message.lParam);
        } else if (message.message == WM_FINISHED) {
            delete reinterpret_cast<CaptureOutcome*>(message.lParam);
        }
    }
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CREATE: {
            app.window = window;
            create_controls();
            refresh_devices();
            SetTimer(window, REFRESH_TIMER, 250, nullptr);
            return 0;
        }
        case WM_SIZE:
            resize_controls(LOWORD(lparam), HIWORD(lparam));
            return 0;
        case WM_GETMINMAXINFO: {
            auto* size = reinterpret_cast<MINMAXINFO*>(lparam);
            size->ptMinTrackSize.x = 850;
            size->ptMinTrackSize.y = 560;
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wparam)) {
                case ID_REFRESH:
                    refresh_devices();
                    return 0;
                case ID_START:
                    start_capture();
                    return 0;
                case ID_STOP:
                    app.running.store(false);
                    EnableWindow(app.stop_button, FALSE);
                    return 0;
                default:
                    break;
            }
            break;
        case WM_NOTIFY: {
            const auto* notice = reinterpret_cast<const NMHDR*>(lparam);
            if (notice != nullptr && notice->idFrom == ID_PACKETS &&
                notice->code == LVN_ITEMCHANGED) {
                show_details();
            }
            return 0;
        }
        case WM_PACKET: {
            std::unique_ptr<netlab::PacketRecord> record(
                reinterpret_cast<netlab::PacketRecord*>(lparam));
            app.pending_events.fetch_sub(1);
            if (record) {
                append_packet(std::move(*record));
            }
            return 0;
        }
        case WM_STATUS: {
            std::unique_ptr<std::string> text(reinterpret_cast<std::string*>(lparam));
            if (text && !app.active) {
                set_status(widen(*text));
            }
            return 0;
        }
        case WM_FINISHED: {
            std::unique_ptr<CaptureOutcome> outcome(
                reinterpret_cast<CaptureOutcome*>(lparam));
            finish_capture(outcome.get());
            return 0;
        }
        case WM_TIMER:
            if (wparam == REFRESH_TIMER) {
                update_running_status();
            }
            return 0;
        case WM_CLOSE:
            app.running.store(false);
            if (app.worker.joinable()) {
                app.worker.join();
            }
            discard_pending_messages();
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            KillTimer(window, REFRESH_TIMER);
            DeleteObject(app.font);
            DeleteObject(app.title_font);
            if (app.network_initialized) {
                WSACleanup();
            }
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

}

// 创建原生 Win32 界面并运行消息循环。
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    std::string runtime_error;
    if (!netlab::prepare_npcap(runtime_error)) {
        MessageBoxW(nullptr, widen(runtime_error).c_str(), L"Npcap 初始化失败", MB_ICONERROR);
        return 1;
    }
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        MessageBoxW(nullptr, L"WinSock 初始化失败。", L"错误", MB_ICONERROR);
        return 1;
    }
    app.network_initialized = true;

    char errbuf[PCAP_ERRBUF_SIZE]{};
    if (pcap_init(PCAP_CHAR_ENC_UTF_8, errbuf) != 0) {
        MessageBoxW(nullptr, widen(errbuf).c_str(), L"Npcap 初始化失败", MB_ICONERROR);
        WSACleanup();
        app.network_initialized = false;
        return 1;
    }

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);

    const wchar_t* class_name = L"Nankai_Network_Lab2_Capture";
    WNDCLASSW wc{};
    wc.hInstance = instance;
    wc.lpfnWndProc = window_proc;
    wc.lpszClassName = class_name;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_WINDOW + 1));

    if (!RegisterClassW(&wc)) {
        WSACleanup();
        return 1;
    }

    HWND window = CreateWindowExW(0, class_name,
                                  L"计算机网络实验二 - Npcap 数据包捕获",
                                  WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 1140, 760,
                                  nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
        WSACleanup();
        return 1;
    }

    ShowWindow(window, show);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
