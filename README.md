# SocketChat

用 C++ 编写的 TCP 多人聊天室，包含服务器、命令行客户端和 Win32 图形客户端。

## 使用方法

在 Visual Studio 的 **x64 Native Tools Command Prompt** 中编译：

```bat
cd /d D:\Study\NetworkLab1\SocketChat
build.bat
build_gui.bat
```

在 PowerShell 中启动演示，会打开服务器和 Alice、Bob、Charlie 三个客户端：

```powershell
cd D:\Study\NetworkLab1\SocketChat
powershell -ExecutionPolicy Bypass -File .\scripts\open_chat_windows.ps1
```

在每个窗口点击“连接服务器”。默认地址为 `127.0.0.1`，端口为 `8888`。选择“公共聊天室”发送公聊消息，点击在线用户进入私聊，在群组页面填写群名、勾选成员后建立群聊。聊天记录显示在上方，连接提示和成员变动显示在下方。

关闭某个客户端后，可以单独重新打开，例如：

```powershell
Start-Process .\bin\gui_client.exe -ArgumentList '--name Bob'
```

也可以分别启动服务器和命令行客户端：

```powershell
.\bin\server.exe 8888
```

在另一个 PowerShell 窗口中输入：

```powershell
cd D:\Study\NetworkLab1\SocketChat
.\bin\client.exe 127.0.0.1 8888 Alice
```

直接输入文字发送公聊消息；输入 `/pm Bob 内容` 发送私聊，`/group create Study` 建群，`/group join Study` 入群，`/g Study 内容` 发送群消息，`/group leave Study` 退群，`/list` 显示在线用户，`/groups` 显示群列表，`/quit` 退出。昵称和群名支持字母、数字、下划线，长度为 1～16 个字符。

在 MSVC 开发者环境中运行自动测试：

```powershell
powershell -ExecutionPolicy Bypass -File .\run_tests_windows.ps1
build_gui_tests.bat
powershell -ExecutionPolicy Bypass -File .\scripts\run_gui_tests_windows.ps1
```

测试日志保存在 `evidence/logs`。使用 Wireshark 抓取本机演示时，选择 Npcap 回环接口，填写显示过滤器 `tcp.port == 8888`，在连接客户端前开始捕获，演示结束后保存为 `.pcapng`。

## 功能设计

采用 C/S 架构，由服务器接收连接、管理用户和群组、转发消息，每条客户端连接使用一个工作线程。公聊发送给所有在线用户，私聊发送给聊天双方，群聊发送给群成员，转发时加入服务器时间。

网络通信直接调用 Winsock2 的 `socket`、`bind`、`listen`、`accept`、`connect`、`send` 和 `recv` 等基本函数。使用 UTF-8 传送中英文，以换行符划分消息，在接收缓存中拼接完整消息；发送时循环处理尚未写出的字节，并用发送锁保持同一连接上的消息顺序。

- `src/server.cpp`：连接处理、登录、消息转发、群成员管理、退出。
- `src/client.cpp`：命令行输入、接收线程、消息显示。
- `src/gui_client.cpp`：原生 Win32 窗口、会话切换、成员选择、聊天记录、连接动态。
- `src/protocol.h`、`src/protocol.cpp`：消息格式、分帧、完整发送、UTF-8 校验、时间戳。
- `src/net_platform.h`：套接字初始化、关闭和平台差异处理。
- `tests/`：协议测试、网络集成测试、界面布局测试。
- `docs/`：报告的 LaTeX 源文件、截图和代码片段，使用 XeLaTeX 或 Tectonic 编译 `main.tex`。

窗口中的待回显数用于观察应用消息是否收到回应。TCP 重传等传输现象在 Wireshark 中观察。
