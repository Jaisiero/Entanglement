$date = Get-Date -Format "yyyyMMdd_HHmmss_fff"

$logDir = "$PSScriptRoot\log"
if (-not (Test-Path $logDir)) {
    New-Item -ItemType Directory -Path $logDir | Out-Null
}

$serverLog = "$logDir\soak_server_$date.log"
$clientLog = "$logDir\soak_client_$date.log"

Write-Host "Starting soak test at $date"
Write-Host "Server log: $serverLog"
Write-Host "Client log: $clientLog"

$server = Start-Process -FilePath ".\build\release-simloss\Release\EntanglementServer.exe" `
    -ArgumentList "-d 5 -w 16" `
    -RedirectStandardOutput $serverLog `
    -NoNewWindow -PassThru

Start-Sleep -Seconds 1

$client = Start-Process -FilePath ".\build\release-simloss\Release\EntanglementClient.exe" `
    -ArgumentList "-t 30 -c 128 -d 5" `
    -RedirectStandardOutput $clientLog `
    -NoNewWindow -PassThru

Write-Host "Waiting for client (PID $($client.Id)) to finish..."
$client.WaitForExit()
Write-Host "Client finished with exit code $($client.ExitCode)"

Write-Host "Waiting for server (PID $($server.Id)) to finish..."
$server.WaitForExit()
Write-Host "Server finished with exit code $($server.ExitCode)"

Write-Host "Soak test complete. Logs in $logDir"
