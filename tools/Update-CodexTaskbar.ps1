param(
    [string]$Tag = "",
    [string]$TargetPath = (Join-Path $PSScriptRoot "CodexTaskbar.exe"),
    [switch]$NoRestart
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$repo = "nagaho5/CodexTaskbar"
$apiBase = "https://api.github.com/repos/$repo/releases"
$headers = @{
    "User-Agent" = "CodexTaskbar-Updater"
    "Accept" = "application/vnd.github+json"
}

function Get-Release {
    if ([string]::IsNullOrWhiteSpace($Tag)) {
        return Invoke-RestMethod -Uri "$apiBase/latest" -Headers $headers
    }

    $escapedTag = [System.Uri]::EscapeDataString($Tag)
    return Invoke-RestMethod -Uri "$apiBase/tags/$escapedTag" -Headers $headers
}

function Get-AssetUrl($release, [string]$name) {
    $asset = $release.assets | Where-Object { $_.name -eq $name } | Select-Object -First 1
    if (-not $asset) {
        throw "Release $($release.tag_name) does not contain $name."
    }
    return $asset.browser_download_url
}

$release = Get-Release
$exeUrl = Get-AssetUrl $release "CodexTaskbar.exe"
$shaUrl = Get-AssetUrl $release "SHA256.txt"

$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("CodexTaskbarUpdate-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempRoot | Out-Null

$tempExe = Join-Path $tempRoot "CodexTaskbar.exe"
$tempSha = Join-Path $tempRoot "SHA256.txt"
$backupPath = "$TargetPath.bak"

try {
    Write-Host "Downloading CodexTaskbar $($release.tag_name)..."
    Invoke-WebRequest -Uri $exeUrl -Headers $headers -OutFile $tempExe -UseBasicParsing
    Invoke-WebRequest -Uri $shaUrl -Headers $headers -OutFile $tempSha -UseBasicParsing

    $shaText = Get-Content -Raw -Path $tempSha
    $match = [regex]::Match($shaText, "(?im)^([0-9a-f]{64})\s+CodexTaskbar\.exe\s*$")
    if (-not $match.Success) {
        throw "SHA256.txt does not contain a valid CodexTaskbar.exe checksum."
    }

    $expected = $match.Groups[1].Value.ToLowerInvariant()
    $actual = (Get-FileHash -Path $tempExe -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $expected) {
        throw "SHA-256 mismatch. Expected $expected, got $actual. Nothing was replaced."
    }

    Write-Host "SHA-256 verified: $actual"

    if (Get-Command Unblock-File -ErrorAction SilentlyContinue) {
        Unblock-File -Path $tempExe
    }

    $targetDirectory = Split-Path -Parent $TargetPath
    if ($targetDirectory -and -not (Test-Path $targetDirectory)) {
        New-Item -ItemType Directory -Path $targetDirectory -Force | Out-Null
    }

    Get-Process -Name "CodexTaskbar" -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue

    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while ((Get-Process -Name "CodexTaskbar" -ErrorAction SilentlyContinue) -and
           [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 100
    }

    if (Test-Path $backupPath) {
        Remove-Item -Force $backupPath
    }
    if (Test-Path $TargetPath) {
        Move-Item -Force $TargetPath $backupPath
    }

    try {
        Copy-Item -Force $tempExe $TargetPath
    }
    catch {
        if ((Test-Path $backupPath) -and -not (Test-Path $TargetPath)) {
            Move-Item -Force $backupPath $TargetPath
        }
        throw
    }

    if (Test-Path $backupPath) {
        Remove-Item -Force $backupPath
    }

    Write-Host "Updated: $TargetPath"
    if (-not $NoRestart) {
        Start-Process -FilePath $TargetPath | Out-Null
        Write-Host "CodexTaskbar restarted."
    }
}
finally {
    Remove-Item -Recurse -Force $tempRoot -ErrorAction SilentlyContinue
}
