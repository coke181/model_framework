[CmdletBinding()]
param(
    [switch]$Build,
    [ValidateSet('in_memory', 'mysql_redis')]
    [string]$StorageMode = 'in_memory',
    [int]$Port = 8080
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$exe = Join-Path $projectRoot 'build-tracy/Release/hello_service.exe'
$profiler = 'D:\vcpkg\installed\x64-windows\tools\tracy\tracy-profiler.exe'
$stdoutLog = Join-Path $projectRoot 'build-tracy/hello_service.stdout.log'
$stderrLog = Join-Path $projectRoot 'build-tracy/hello_service.stderr.log'

if ($Build) {
    & cmake -S $projectRoot -B (Join-Path $projectRoot 'build-tracy') -A x64 `
        -DMF_ENABLE_OTEL=OFF -DMF_ENABLE_TRACY=ON `
        -DCMAKE_PREFIX_PATH=D:/vcpkg/installed/x64-windows
    if ($LASTEXITCODE -ne 0) { throw 'Tracy CMake configuration failed.' }
    & cmake --build (Join-Path $projectRoot 'build-tracy') --config Release --target hello_service
    if ($LASTEXITCODE -ne 0) { throw 'Tracy Release build failed.' }
}

if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "Missing $exe. Run this script with -Build first."
}
if (-not (Test-Path -LiteralPath $profiler -PathType Leaf)) {
    throw "Missing $profiler. Install tracy[gui-tools]:x64-windows first."
}

$exe = [System.IO.Path]::GetFullPath($exe)
$profiler = [System.IO.Path]::GetFullPath($profiler)
$service = Get-CimInstance Win32_Process -Filter "Name = 'hello_service.exe'" |
    Where-Object { $_.ExecutablePath -and [System.StringComparer]::OrdinalIgnoreCase.Equals($_.ExecutablePath, $exe) } |
    Select-Object -First 1

if (-not $service) {
    $occupied = Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($occupied) {
        throw "Port $Port is already in use (PID $($occupied.OwningProcess)). Stop that service before profiling."
    }

    $env:PATH = "D:\OpenSSL-Win64\bin;D:\vcpkg\installed\x64-windows\bin;$env:PATH"
    $env:HELLO_SERVICE_HOST = '127.0.0.1'
    $env:HELLO_SERVICE_PORT = [string]$Port
    $env:HELLO_SERVICE_STORAGE_MODE = $StorageMode
    $env:HELLO_SERVICE_ENV = 'development'

    $service = Start-Process -FilePath $exe -WorkingDirectory $projectRoot `
        -RedirectStandardOutput $stdoutLog -RedirectStandardError $stderrLog `
        -WindowStyle Hidden -PassThru

    $ready = $false
    for ($attempt = 0; $attempt -lt 30; ++$attempt) {
        Start-Sleep -Milliseconds 300
        if ($service.HasExited) { break }
        try {
            $response = Invoke-WebRequest -Uri "http://127.0.0.1:$Port/health" `
                -UseBasicParsing -TimeoutSec 1
            if ($response.StatusCode -eq 200) { $ready = $true; break }
        } catch {
            # Keep polling until the server is ready or exits.
        }
    }
    if (-not $ready) {
        Write-Host "Service did not become ready. Logs: $stdoutLog, $stderrLog"
        if (Test-Path -LiteralPath $stderrLog) { Get-Content -LiteralPath $stderrLog -Tail 20 }
        throw 'hello_service startup failed.'
    }
    Write-Host "Tracy service running: PID=$($service.Id), http://127.0.0.1:$Port/health"
} else {
    Write-Host "Reusing Tracy service: PID=$($service.ProcessId)"
}

$runningProfiler = Get-CimInstance Win32_Process -Filter "Name = 'tracy-profiler.exe'" |
    Where-Object { $_.ExecutablePath -and [System.StringComparer]::OrdinalIgnoreCase.Equals($_.ExecutablePath, $profiler) } |
    Select-Object -First 1
if ($runningProfiler) {
    Write-Host "Tracy Profiler is already open: PID=$($runningProfiler.ProcessId)"
} else {
    $viewer = Start-Process -FilePath $profiler -WorkingDirectory (Split-Path -Parent $profiler) `
        -ArgumentList @('-a', '127.0.0.1', '-p', '8086') -PassThru
    Write-Host "Tracy Profiler started: PID=$($viewer.Id), connecting to 127.0.0.1:8086"
}

Write-Host "Service logs: $stdoutLog, $stderrLog"
