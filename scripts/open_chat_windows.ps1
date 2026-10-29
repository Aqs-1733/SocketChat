param([int]$Port = 8888)

$project = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$server = Join-Path $project 'bin\server.exe'
$gui = Join-Path $project 'bin\gui_client.exe'
if (!(Test-Path $server) -or !(Test-Path $gui)) {
    Write-Error '请先运行 build.bat 和 build_gui.bat。'
    exit 1
}

Start-Process -FilePath 'powershell.exe' -WorkingDirectory $project -ArgumentList @('-NoProfile', '-NoExit', '-Command', "& '$server' $Port")
Start-Sleep -Milliseconds 700
foreach ($name in @('Alice', 'Bob', 'Charlie')) {
    Start-Process -FilePath $gui -WorkingDirectory $project -ArgumentList "--name $name"
    Start-Sleep -Milliseconds 220
}
Write-Output '已启动服务器和 Alice、Bob、Charlie 三个图形客户端。'
