# SocketChat

C++ TCP 聊天程序，使用 Winsock2 和基础 Socket 函数实现。

## 功能

- 命令行客户端和 Win32 图形客户端
- 多人聊天、中文消息、服务器时间戳
- TCP 拆包和粘包处理

## 编译

在 Visual Studio 的 x64 开发者命令行中运行：

```bat
build.bat
build_gui.bat
```

## 运行

```bat
bin\server.exe 8888
bin\client.exe 127.0.0.1 8888 Alice
```

图形客户端：`bin\gui_client.exe`

## 测试

```powershell
.\run_tests_windows.ps1
```
