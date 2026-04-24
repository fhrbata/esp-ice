# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
#
# esp-ice installer for Windows (PowerShell 5.1+ / PowerShell 7+).
#
# Usage:
#   irm https://raw.githubusercontent.com/fhrbata/esp-ice/main/install.ps1 | iex
#
# Environment overrides:
#   $env:ICE_VERSION       Release tag (default: latest). Accepts "v0.2.0" or "0.2.0".
#   $env:ICE_INSTALL_DIR   Destination directory
#                          (default: $env:LOCALAPPDATA\Programs\ice\bin).
#   $env:ICE_ARCH          Override detected architecture.
#   $env:ICE_REPO          GitHub repository (default: fhrbata/esp-ice).

$ErrorActionPreference = 'Stop'

$repo    = if ($env:ICE_REPO)    { $env:ICE_REPO }    else { 'fhrbata/esp-ice' }
$version = if ($env:ICE_VERSION) { $env:ICE_VERSION } else { 'latest' }
$installDir = if ($env:ICE_INSTALL_DIR) {
    $env:ICE_INSTALL_DIR
} else {
    Join-Path $env:LOCALAPPDATA 'Programs\ice\bin'
}

$arch = if ($env:ICE_ARCH) {
    $env:ICE_ARCH
} else {
    switch ($env:PROCESSOR_ARCHITECTURE) {
        'AMD64' { 'amd64' }
        'ARM64' { 'arm64' }
        'x86'   { 'i386' }
        default { throw "unsupported architecture: $env:PROCESSOR_ARCHITECTURE" }
    }
}

if ($version -eq 'latest') {
    Write-Host 'Resolving latest version...'
    $rel = Invoke-RestMethod "https://api.github.com/repos/$repo/releases/latest"
    $ver = $rel.tag_name -replace '^v', ''
} else {
    $ver = $version -replace '^v', ''
}

$pkg = "ice-$ver-win-$arch.zip"
$url = "https://github.com/$repo/releases/download/v$ver/$pkg"

$tmp = Join-Path $env:TEMP "ice-install-$([Guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
    Write-Host "Downloading $pkg..."
    $zip = Join-Path $tmp $pkg
    Invoke-WebRequest -Uri $url -OutFile $zip

    Write-Host 'Extracting...'
    Expand-Archive -Path $zip -DestinationPath $tmp -Force

    New-Item -ItemType Directory -Path $installDir -Force | Out-Null
    $src = Join-Path $tmp "ice-$ver\bin\ice.exe"
    $dst = Join-Path $installDir 'ice.exe'
    Copy-Item -Path $src -Destination $dst -Force
} finally {
    Remove-Item -Path $tmp -Recurse -Force -ErrorAction SilentlyContinue
}

$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
$pathUpdated = $false
if (-not $userPath -or ($userPath -split ';' -notcontains $installDir)) {
    $newPath = if ($userPath) { "$userPath;$installDir" } else { $installDir }
    [Environment]::SetEnvironmentVariable('Path', $newPath, 'User')
    $pathUpdated = $true
}

@"

  ice $ver installed
  ───────────────────────────────────────────────
  path:    $dst
  version: $ver
  target:  win-$arch

Next steps:
"@ | Write-Host

$step = 1
if ($pathUpdated) {
    @"

  $step. $installDir was added to your user PATH.
     Restart your shell, or refresh this session:

       `$env:Path += ';$installDir'
"@ | Write-Host
    $step++
}

@"

  $step. Enable tab completion.  Every TAB re-invokes ice to list
     subcommands, flags, chip targets, and config keys, so every step
     below is discoverable by pressing TAB:

       ice completion powershell | Out-String | Invoke-Expression

     To persist it across sessions, append the same line to your
     PowerShell profile:

       Add-Content -Path `$PROFILE -Value 'ice completion powershell | Out-String | Invoke-Expression'
"@ | Write-Host
$step++

@"

  $step. Run the built-in getting-started guide -- it walks you
     from zero to a flashed hello_world:

       ice docs getting-started

Note: the binary is currently unsigned (PoC).  On first launch,
Windows SmartScreen may warn "Windows protected your PC" -- click
"More info" then "Run anyway".
"@ | Write-Host
