#!/usr/bin/env pwsh
# -----------------------------------------------------------------------------
# deploy.ps1  - Deploy the Strava Bridge container to the remote Docker host
#
# Usage:
#   .\deploy.ps1
#   .\deploy.ps1 -RemoteHost merlin@192.168.1.54
#   .\deploy.ps1 -RemoteHost merlin@192.168.1.54 -RemoteDir /home/merlin/strava-bridge
#   .\deploy.ps1 -UseSudo
#   .\deploy.ps1 -UseSudo -AllowSudoPrompt
#
# Requirements on the local machine:
#   - ssh / scp (OpenSSH client, built into Windows 10+ and PowerShell 7+)
#   - SSH key already authorized on the remote host  (ssh-copy-id merlin@192.168.1.54)
#
# Requirements on the remote host:
#   - Docker + Docker Compose plugin (docker compose)
# -----------------------------------------------------------------------------

param(
    [string]$RemoteHost = "merlin@192.168.1.54",
    [string]$RemoteDir  = "/home/merlin/strava-bridge",
    [switch]$UseSudo,
    [switch]$AllowSudoPrompt
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ($AllowSudoPrompt -and -not $UseSudo) {
    throw "-AllowSudoPrompt requires -UseSudo."
}

function Assert-LastExitCode {
    param(
        [string]$Action,
        [string]$Hint = ""
    )

    if ($LASTEXITCODE -ne 0) {
        $message = "$Action failed with exit code $LASTEXITCODE."
        if ($Hint) {
            $message = "$message $Hint"
        }

        throw $message
    }
}

function Invoke-RemoteCommand {
    param(
        [string]$Command,
        [string]$Action,
        [switch]$AllocateTty,
        [string]$FailureHint = ""
    )

    $sshArgs = @()
    if ($AllocateTty) {
        $sshArgs += "-t"
    }
    $sshArgs += $RemoteHost
    $sshArgs += $Command

    & ssh @sshArgs
    Assert-LastExitCode -Action $Action -Hint $FailureHint
}

# Files to deploy (relative to this script's directory)
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$FilesToCopy = @(
    "strava_bridge.py",
    "Dockerfile",
    "docker-compose.strava-bridge.yml"
)

$DockerComposePrefix = ""
if ($UseSudo) {
    $DockerComposePrefix = if ($AllowSudoPrompt) { "sudo" } else { "sudo -n" }
}

$RemoteDockerCompose = if ($DockerComposePrefix) {
    "$DockerComposePrefix docker compose"
} else {
    "docker compose"
}

$SudoFailureHint = if ($UseSudo -and -not $AllowSudoPrompt) {
    "If the remote host requires a sudo password, rerun with -UseSudo -AllowSudoPrompt or configure passwordless sudo for docker."
} else {
    ""
}

Write-Host ""
Write-Host "=====================================================" -ForegroundColor Cyan
Write-Host "  Strava Bridge  -  Deploy to $RemoteHost" -ForegroundColor Cyan
Write-Host "=====================================================" -ForegroundColor Cyan
Write-Host ""

# 1. Ensure the remote directory exists
Write-Host "[1/4] Creating remote directory $RemoteDir ..." -ForegroundColor Yellow
Invoke-RemoteCommand -Command "mkdir -p '$RemoteDir'" -Action "Creating remote directory"
Write-Host "      OK" -ForegroundColor Green

# 2. Copy files
Write-Host "[2/4] Copying files ..." -ForegroundColor Yellow
for ($index = 0; $index -lt $FilesToCopy.Count; $index++) {
    $deployFile = $FilesToCopy[$index]
    $localPath = Join-Path $ScriptDir $deployFile
    if (-not (Test-Path $localPath)) {
        Write-Error "File not found: $localPath"
        exit 1
    }
    Write-Host "      $deployFile" -ForegroundColor Gray
    scp "$localPath" "${RemoteHost}:${RemoteDir}/${deployFile}"
    Assert-LastExitCode "Copying $deployFile"
}
Write-Host "      OK" -ForegroundColor Green

# 3. Build and (re)start the container
Write-Host "[3/4] Building image and starting container ..." -ForegroundColor Yellow
Invoke-RemoteCommand -Command "cd '$RemoteDir' && $RemoteDockerCompose -f docker-compose.strava-bridge.yml up -d --build --remove-orphans" -Action "Building and starting the container" -AllocateTty:$AllowSudoPrompt -FailureHint $SudoFailureHint
Write-Host "      OK" -ForegroundColor Green

# 4. Show running status
Write-Host "[4/4] Container status:" -ForegroundColor Yellow
Invoke-RemoteCommand -Command "$RemoteDockerCompose -f '$RemoteDir/docker-compose.strava-bridge.yml' ps" -Action "Retrieving container status" -AllocateTty:$AllowSudoPrompt -FailureHint $SudoFailureHint

Write-Host ""
Write-Host "=====================================================" -ForegroundColor Cyan
Write-Host "  Done.  Bridge endpoint:" -ForegroundColor Cyan
Write-Host "  http://192.168.1.54:8082/api/exercise-load" -ForegroundColor White
Write-Host "=====================================================" -ForegroundColor Cyan
Write-Host ""
