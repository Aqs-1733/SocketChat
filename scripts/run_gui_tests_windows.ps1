# 启动独立服务器，检查原生窗口布局和真实收发。
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    cmd /c build_gui_tests.bat
    if ($LASTEXITCODE -ne 0) { throw 'GUI 测试编译失败' }
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $port = $listener.LocalEndpoint.Port
    $listener.Stop()
    New-Item -ItemType Directory -Force -Path 'evidence\logs' | Out-Null
    $server = Start-Process -FilePath "$root\bin\server.exe" -ArgumentList $port -PassThru -WindowStyle Hidden
    try {
        Start-Sleep -Milliseconds 700
        if ($server.HasExited) { throw '测试服务器启动失败' }
        & "$root\bin\gui_layout_tests.exe" $port | Tee-Object -FilePath 'evidence\logs\gui_layout_tests.txt'
        if ($LASTEXITCODE -ne 0) { throw 'GUI 测试失败' }
    } finally {
        if (-not $server.HasExited) { Stop-Process -Id $server.Id -Force }
    }
} finally {
    Pop-Location
}
