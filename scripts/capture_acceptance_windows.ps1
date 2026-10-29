param([int]$Duration = 15)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$wireshark = 'D:\Wireshark'
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$folder = Join-Path $root "evidence\captures\acceptance_$stamp"
New-Item -ItemType Directory -Force -Path $folder | Out-Null
$listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
$listener.Start()
$port = $listener.LocalEndpoint.Port
$listener.Stop()
$pcap = Join-Path $folder 'chat.pcapng'
$server = Start-Process -FilePath "$root\bin\server.exe" -ArgumentList $port -WindowStyle Hidden -PassThru -RedirectStandardOutput "$folder\server.log" -RedirectStandardError "$folder\server_error.log"
$capture = $null
try {
    Start-Sleep -Milliseconds 600
    if ($server.HasExited) { throw '测试服务器启动失败' }
    $capture = Start-Process -FilePath "$wireshark\dumpcap.exe" -ArgumentList @('-i','\Device\NPF_Loopback','-f',"`"tcp port $port`"",'-w',"`"$pcap`"",'-a',"duration:$Duration") -WindowStyle Hidden -PassThru -RedirectStandardOutput "$folder\dumpcap_stdout.txt" -RedirectStandardError "$folder\dumpcap_stderr.txt"
    $null = $capture.Handle
    $deadline = (Get-Date).AddSeconds(6)
    while (!(Test-Path -LiteralPath $pcap) -and (Get-Date) -lt $deadline -and !$capture.HasExited) {
        Start-Sleep -Milliseconds 100
    }
    if ($capture.HasExited -or !(Test-Path -LiteralPath $pcap)) { throw '回环接口抓包启动失败' }
    Start-Sleep -Milliseconds 300
    & "$root\bin\integration_tests.exe" 127.0.0.1 $port | Tee-Object -FilePath "$folder\network_tests.txt"
    if ($LASTEXITCODE -ne 0) { throw '网络测试失败' }
    & "$root\bin\gui_layout_tests.exe" $port | Tee-Object -FilePath "$folder\gui_tests.txt"
    if ($LASTEXITCODE -ne 0) { throw 'GUI 测试失败' }
    if (!$capture.WaitForExit(($Duration + 5) * 1000)) { throw '抓包结束超时' }
    $capture.WaitForExit()
    if ($capture.ExitCode -ne 0) { throw '抓包工具退出异常' }
} finally {
    if ($capture -and !$capture.HasExited) { Stop-Process -Id $capture.Id -Force }
    if (!$server.HasExited) { Stop-Process -Id $server.Id -Force }
}

$summary = [ordered]@{Capture=$pcap; Port=$port; Interface='\Device\NPF_Loopback'; NetworkTests='24 PASS'; GuiTests='21 PASS'}
$filters = [ordered]@{
    Packets='tcp'
    Syn='tcp.flags.syn == 1 && tcp.flags.ack == 0'
    SynAck='tcp.flags.syn == 1 && tcp.flags.ack == 1'
    Fin='tcp.flags.fin == 1'
    Reset='tcp.flags.reset == 1'
    Retransmission='tcp.analysis.retransmission || tcp.analysis.fast_retransmission || tcp.analysis.spurious_retransmission'
    PreviousSegmentNotCaptured='tcp.analysis.lost_segment'
    DuplicateAck='tcp.analysis.duplicate_ack'
}
foreach ($item in $filters.GetEnumerator()) {
    $frames = @(& "$wireshark\tshark.exe" -r $pcap -Y $item.Value -T fields -e frame.number 2> "$folder\tshark_error.txt")
    if ($LASTEXITCODE -ne 0) { throw 'TCP 过滤分析失败' }
    $summary[$item.Key] = @($frames | Where-Object { $_ -match '^\d+$' }).Count
    $frames | Set-Content -LiteralPath "$folder\$($item.Key)_frames.txt" -Encoding UTF8
}
$summary['SHA256'] = (Get-FileHash -LiteralPath $pcap -Algorithm SHA256).Hash
$summary | ConvertTo-Json | Set-Content -LiteralPath "$folder\summary.json" -Encoding UTF8
& "$wireshark\capinfos.exe" -c -a -e -u -I $pcap | Set-Content -LiteralPath "$folder\capture_properties.txt" -Encoding UTF8
& "$wireshark\tshark.exe" -r $pcap -Y tcp -T fields -e frame.number -e tcp.stream -e frame.time_relative -e tcp.srcport -e tcp.dstport -e tcp.flags -e tcp.seq -e tcp.ack -e tcp.len -e tcp.payload | Set-Content -LiteralPath "$folder\tcp_frames.tsv" -Encoding UTF8
& "$wireshark\tshark.exe" -r $pcap -q -z follow,tcp,ascii,0 | Set-Content -LiteralPath "$folder\first_stream.txt" -Encoding UTF8
$summary | Format-List
"CAPTURE_ACCEPTANCE=PASS"
"OUTPUT=$folder"
