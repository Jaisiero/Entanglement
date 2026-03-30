# ============================================================================
# Entanglement Benchmark Suite
# Tests theoretical vs real limits on current hardware
# ============================================================================

param(
    [string]$ServerExe = ".\build\EntanglementServer.exe",
    [string]$StressExe = ".\build\EntanglementStress.exe",
    [int]$Duration = 20,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Continue"

function Kill-Servers {
    Stop-Process -Name "EntanglementServer" -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1
}

function Run-Test {
    param(
        [int]$Workers,
        [int]$Clients,
        [int]$Rate,
        [int]$Seconds,
        [string]$Label
    )

    Kill-Servers

    # Start server
    $serverArgs = @("-w", $Workers)
    $proc = Start-Process -FilePath $ServerExe -ArgumentList $serverArgs `
        -PassThru -WindowStyle Hidden
    Start-Sleep -Seconds 2

    if ($proc.HasExited) {
        Write-Host "  [SKIP] Server failed to start" -ForegroundColor Red
        return $null
    }

    # Run stress test and capture output
    $output = & $StressExe -c $Clients -t $Seconds -r $Rate 2>&1 | Out-String

    Kill-Servers

    # Parse results
    $result = @{
        Label      = $Label
        Workers    = $Workers
        Clients    = $Clients
        Rate       = $Rate
        TargetPPS  = $Clients * $Rate
    }

    # Extract key metrics
    if ($output -match "Connected:\s+(\d+)") { $result.Connected = [int]$Matches[1] }
    if ($output -match "Losses detected:\s+(\d+)") { $result.Losses = [int]$Matches[1] }
    if ($output -match "Avg send rate:\s+(\d+)") { $result.ActualPPS = [int]$Matches[1] }
    if ($output -match "RESULT:\s+(PASS|DEGRADED)") { $result.Verdict = $Matches[1] }
    if ($output -match "Connection failures:(\d+)") { $result.ConnFail = [int]$Matches[1] }
    if ($output -match "Still connected:\s+(\d+)") { $result.StillConn = [int]$Matches[1] }

    # Extract periodic reports for trend analysis
    $periodics = @()
    $output -split "`n" | ForEach-Object {
        if ($_ -match "\[(\d+)s\].*rate=(\d+)\s*pkt/s\s*losses=(\d+)") {
            $periodics += @{Time=[int]$Matches[1]; Rate=[int]$Matches[2]; Losses=[int]$Matches[3]}
        }
    }
    $result.Periodics = $periodics

    return $result
}

function Print-Result {
    param($r)
    if ($null -eq $r) { return }

    $color = if ($r.Verdict -eq "PASS") { "Green" } else { "Yellow" }
    $ratio = if ($r.TargetPPS -gt 0) { [math]::Round(($r.ActualPPS / $r.TargetPPS) * 100, 1) } else { 0 }

    Write-Host ("  {0,-35} W={1} C={2,5} Target={3,6} Actual={4,6} pkt/s ({5,5}%)  Loss={6,-6} [{7}]" -f `
        $r.Label, $r.Workers, $r.Clients, $r.TargetPPS, $r.ActualPPS, $ratio, $r.Losses, $r.Verdict) `
        -ForegroundColor $color

    # Show trend if degrading
    if ($r.Periodics.Count -ge 3) {
        $first = $r.Periodics[0].Rate
        $last  = $r.Periodics[-1].Rate
        if ($last -lt $first * 0.85) {
            Write-Host ("    ^^ DEGRADING: {0} -> {1} pkt/s over time" -f $first, $last) -ForegroundColor Red
        }
    }
}

# ============================================================================
Write-Host ""
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host " Entanglement Benchmark Suite" -ForegroundColor Cyan
Write-Host " CPU: $((Get-CimInstance Win32_Processor).Name)"
Write-Host " Cores: $((Get-CimInstance Win32_Processor).NumberOfCores) / Threads: $((Get-CimInstance Win32_Processor).NumberOfLogicalProcessors)"
Write-Host " RAM: $([math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory/1GB,1)) GB"
Write-Host " MAX_CONNECTIONS: 4096"
Write-Host " Duration per test: ${Duration}s"
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host ""

$allResults = @()

# ============================================================================
# TEST 1: Worker scaling (fixed 500 clients, 10 Hz)
# ============================================================================
Write-Host "=== TEST 1: Worker Scaling (500 clients @ 10 Hz = 5000 pkt/s) ===" -ForegroundColor Cyan
foreach ($w in @(1, 2, 4, 8)) {
    $r = Run-Test -Workers $w -Clients 500 -Rate 10 -Seconds $Duration -Label "Workers=$w, 500c@10Hz"
    Print-Result $r
    $allResults += $r
}
Write-Host ""

# ============================================================================
# TEST 2: Connection scaling (4 workers, increasing clients)
# ============================================================================
Write-Host "=== TEST 2: Connection Scaling (4 workers, 5 Hz) ===" -ForegroundColor Cyan
foreach ($c in @(500, 1000, 2000, 3000)) {
    $r = Run-Test -Workers 4 -Clients $c -Rate 5 -Seconds $Duration -Label "4W, ${c}c@5Hz"
    Print-Result $r
    $allResults += $r
}
Write-Host ""

# ============================================================================
# TEST 3: Throughput scaling (4 workers, 500 clients, increasing rate)
# ============================================================================
Write-Host "=== TEST 3: Throughput Scaling (4 workers, 500 clients) ===" -ForegroundColor Cyan
foreach ($hz in @(10, 20, 50, 100)) {
    $r = Run-Test -Workers 4 -Clients 500 -Rate $hz -Seconds $Duration -Label "4W, 500c@${hz}Hz"
    Print-Result $r
    $allResults += $r
}
Write-Host ""

# ============================================================================
# TEST 4: Max workers with max clients (8 workers, scaling up)
# ============================================================================
Write-Host "=== TEST 4: 8-Worker Maximum (8 workers, scaling) ===" -ForegroundColor Cyan
foreach ($c in @(1000, 2000, 3000)) {
    $r = Run-Test -Workers 8 -Clients $c -Rate 10 -Seconds $Duration -Label "8W, ${c}c@10Hz"
    Print-Result $r
    $allResults += $r
}
Write-Host ""

# ============================================================================
# SUMMARY
# ============================================================================
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host " BENCHMARK SUMMARY" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan

$passed = ($allResults | Where-Object { $_.Verdict -eq "PASS" }).Count
$total  = $allResults.Count
$maxPPS = ($allResults | Where-Object { $_.Verdict -eq "PASS" } | Sort-Object ActualPPS -Descending | Select-Object -First 1)
$maxConn = ($allResults | Where-Object { $_.Verdict -eq "PASS" } | Sort-Object Clients -Descending | Select-Object -First 1)

Write-Host " Tests passed: $passed / $total"
if ($maxPPS) {
    Write-Host " Max sustained pkt/s (PASS): $($maxPPS.ActualPPS) ($($maxPPS.Label))" -ForegroundColor Green
}
if ($maxConn) {
    Write-Host " Max connections (PASS):     $($maxConn.Clients) ($($maxConn.Label))" -ForegroundColor Green
}

# Memory estimate
Write-Host ""
Write-Host " Memory estimate per connection: ~150 KB (send_buffer + ordered + retransmit)"
Write-Host " 1000 connections = ~150 MB, 4000 = ~600 MB"
Write-Host ""

# Theoretical ceiling
Write-Host " THEORETICAL LIMITS:" -ForegroundColor Yellow
Write-Host "   Single socket: ~300K-500K UDP pkt/s (localhost loopback)"
Write-Host "   Per worker: ~30K connections (hash map + update loop)"
Write-Host "   IOCP batch: 64 completions/dequeue, 128 pre-posted recvs"
Write-Host "   SPSC queue: 8192 slots per worker (~10x 256-batch margin)"
Write-Host "   Congestion window: MAX_CWND=256 per connection"
Write-Host "   Total 8W theoretical: ~50K pkt/s @ 4000 connections"
Write-Host "============================================================" -ForegroundColor Cyan
