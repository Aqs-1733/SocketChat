# 实验二：以太网数据包捕获

使用 C++17、Npcap 和 Win32 捕获本机数据帧，显示源 MAC、目的 MAC、类型字段及其后的第一个字节，支持过滤、定量或定时停止和文本日志。

## 使用

需要 Windows x64 和 Npcap，编译需要 Visual Studio C++ 工具，项目已附 Npcap SDK。

在项目目录运行 `build.bat` 编译，双击 `启动界面.bat` 使用图形界面，选择正在联网的网卡，输入过滤条件和抓包数量后开始捕获，数量为 0 时手动停止，选中记录可查看字段详情。

命令行先列出网卡，再将 N 换成当前网卡编号：

```bat
bin\packet_capture.exe --list
bin\packet_capture.exe --adapter N --filter "arp or ip" --count 5 --duration 10
```

数量或时间先达到哪一项就停止，也可按 Ctrl+C 停止，添加 `--log capture.txt` 保存文本日志，`--help` 查看参数；本实验解析 Ethernet II 帧，Loopback 不适用。
