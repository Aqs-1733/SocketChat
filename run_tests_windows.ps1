# 在已配置 MSVC 的 PowerShell 中运行测试。
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
New-Item -ItemType Directory -Force -Path 'evidence\logs' | Out-Null
cmd /c build.bat
if ($LASTEXITCODE -ne 0) { throw 'C++ 编译或单元测试未通过' }
$port = 18889
$server = Start-Process -FilePath (Join-Path $PSScriptRoot 'bin\server.exe') -ArgumentList $port -PassThru -WindowStyle Hidden -RedirectStandardOutput 'evidence\logs\windows_server.log' -RedirectStandardError 'evidence\logs\windows_server_error.log'
try {
    Start-Sleep -Seconds 2
    & '.\bin\integration_tests.exe' 127.0.0.1 $port 2>&1 | Tee-Object -FilePath 'evidence\logs\windows_cpp_tests.txt'
    if ($LASTEXITCODE -ne 0) { throw 'C++ 集成测试失败，查看日志' }
} finally {
    if (-not $server.HasExited) { Stop-Process -Id $server.Id -Force }
}
Write-Host '[PASS] Windows C++ tests finished. Read evidence/logs/windows_cpp_tests.txt'
