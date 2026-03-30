param(
    [int]$Time        = 30,      # minutes per stage
    [int]$Clients     = 128,     # concurrent clients
    [int]$Drop        = 5,       # simulated loss %
    [int]$Workers     = 16,      # server worker threads
    [string]$Modes    = "rou"    # channel stages (comma-sep: u,r,o,ru,ro,uo,rou)
)

$date = Get-Date -Format "yyyyMMdd_HHmmss_fff"

$logDir = "$PSScriptRoot\log"
if (-not (Test-Path $logDir)) {
    New-Item -ItemType Directory -Path $logDir | Out-Null
}

$serverLog = "$logDir\soak_server_$date.log"
$clientLog = "$logDir\soak_client_$date.log"

Write-Host "=========================================="
Write-Host " Entanglement Soak Test"
Write-Host " Time:     $Time min per stage"
Write-Host " Clients:  $Clients"
Write-Host " Drop:     $Drop%"
Write-Host " Workers:  $Workers"
Write-Host " Modes:    $Modes"
Write-Host " Server:   $serverLog"
Write-Host " Client:   $clientLog"
Write-Host "=========================================="

# Use -k (keep-alive) if multiple stages so server doesn't auto-stop between them
$multiStage = $Modes -match ","
$serverArgs = "-d $Drop -w $Workers"
if ($multiStage) { $serverArgs += " -k" }

$server = Start-Process -FilePath ".\build\release-simloss\Release\EntanglementServer.exe" `
    -ArgumentList $serverArgs `
    -RedirectStandardOutput $serverLog `
    -NoNewWindow -PassThru

Start-Sleep -Seconds 1

$client = Start-Process -FilePath ".\build\release-simloss\Release\EntanglementClient.exe" `
    -ArgumentList "-t $Time -c $Clients -d $Drop -m $Modes -v" `
    -RedirectStandardOutput $clientLog `
    -NoNewWindow -PassThru

Write-Host "Waiting for client (PID $($client.Id)) to finish..."
$client.WaitForExit()
Write-Host "Client finished with exit code $($client.ExitCode)"

Write-Host "Stopping server (PID $($server.Id))..."
Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

Write-Host "Soak test complete. Logs in $logDir"
