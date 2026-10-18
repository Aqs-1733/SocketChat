# 手工功能测试：只负责启动窗口，不替用户生成虚假截图。
param([int]$Port=8888)
$project = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$server = Join-Path $project 'bin\server.exe'
$client = Join-Path $project 'bin\client.exe'
if (!(Test-Path $server) -or !(Test-Path $client)) {
    Write-Error '缺少 bin\server.exe / bin\client.exe。请先运行 build.bat。'
    exit 1
}
Start-Process -FilePath 'powershell.exe' -ArgumentList @('-NoExit','-Command', "& '$server' $Port")
Start-Sleep -Milliseconds 700
foreach ($name in @('Alice','Bob','Charlie')) {
    Start-Process -FilePath 'powershell.exe' -ArgumentList @('-NoExit','-Command', "& '$client' 127.0.0.1 $Port $name")
    Start-Sleep -Milliseconds 220
}
Write-Output '已启动服务器及 3 个客户端窗口。请按截图清单手工保存真实 PNG。'
