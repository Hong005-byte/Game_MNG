# Builds the Windows installer with a version number read straight out of
# Game Mng/Version.h (Version::kGameVersion) instead of needing TycoonIdle.iss's
# own MyAppVersion bumped by hand too -- see that script's own header comment.
# One source of truth: bump Version.h, run this, done.
#
# Usage (from anywhere -- paths below are relative to this script's own
# location, not the caller's cwd):
#   installer\build.ps1
#
# Assumes dist\TycoonIdle\ already has the build you want packaged (see
# README.md's "release a new version" workflow -- copy the freshly-built
# Release exe/DLLs there before running this, same as before).

$ErrorActionPreference = "Stop"

$versionHeaderPath = Join-Path $PSScriptRoot "..\Game Mng\Version.h"
$content = Get-Content $versionHeaderPath -Raw
if ($content -notmatch 'kGameVersion\s*=\s*"([^"]+)"') {
    Write-Error "Could not find kGameVersion in $versionHeaderPath -- is Version.h's own format still 'constexpr const char* kGameVersion = \"X.Y.Z\";'?"
    exit 1
}
$version = $Matches[1]
Write-Host "Building installer for version $version (from Version.h)"

$iscc = "C:\Program Files\Inno Setup 7\ISCC.exe"
if (-not (Test-Path $iscc)) {
    Write-Error "Inno Setup 7's ISCC.exe not found at '$iscc'. Install it (or adjust this path if you have a different version)."
    exit 1
}

$issPath = Join-Path $PSScriptRoot "TycoonIdle.iss"
& $iscc "/DMyAppVersion=$version" $issPath
exit $LASTEXITCODE
