# S5 delivery packaging: stage the release build, hash it, zip it, and write the
# update manifest next to the zip. Signing is intentionally skipped here: the
# 2.0 branch does not sign or publish; the release line on main signs with
# signtool + the project certificate.
#
# Package contents match the "发布打包" section of the root README:
#   HaloCap.exe, onnxruntime.dll, the four VC++ runtime DLLs, models\ppocrv6-tiny-1,
#   README.txt, CHANGELOG.md, THIRD-PARTY-NOTICES.md, version.txt.
# Deliberately excluded from the zip: the full README.md (developer sections such as
# "从源码构建" are irrelevant to end users) and RELEASE-NOTES.md (that is the GitHub
# Release body, kept next to the zip, not inside it).
param(
    [string]$Version = "2.0.0",
    [string]$Output = "dist"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $root
$appDir = Join-Path $root "build-release\app"
$exe = Join-Path $appDir "HaloCap.exe"
$ort = Join-Path $appDir "onnxruntime.dll"
$models = Join-Path $root "models\ppocrv6-tiny-1"

# VC++ runtime DLLs ship alongside the exe so the portable app runs without the
# redistributable installed. The Redist directory is versioned by toolset; pick
# the newest one found.
$redistRoot = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC"
$redist = Get-ChildItem $redistRoot -Directory -ErrorAction SilentlyContinue |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if ($null -eq $redist) {
    throw "VC++ redist not found under $redistRoot"
}
$crt = Join-Path $redist.FullName "x64\Microsoft.VC143.CRT"

foreach ($required in @($exe, $ort, (Join-Path $models "ppocrv6_tiny_det.onnx"),
                        (Join-Path $crt "msvcp140.dll"), (Join-Path $crt "vcruntime140.dll"))) {
    if (-not (Test-Path $required)) {
        throw "missing build input: $required (run build-release.cmd first)"
    }
}

$dist = Join-Path $root $Output
$stage = Join-Path $dist "HaloCap-$Version-win-x64"
$modelsStage = Join-Path $stage "models\ppocrv6-tiny-1"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $modelsStage -Force | Out-Null

Copy-Item $exe (Join-Path $stage "HaloCap.exe") -Force
Copy-Item $ort (Join-Path $stage "onnxruntime.dll") -Force
foreach ($dll in @("msvcp140.dll", "msvcp140_1.dll", "vcruntime140.dll", "vcruntime140_1.dll")) {
    Copy-Item (Join-Path $crt $dll) (Join-Path $stage $dll) -Force
}
Get-ChildItem $models -File | Where-Object { $_.Extension -in @(".onnx", ".txt") } |
    ForEach-Object { Copy-Item $_.FullName (Join-Path $modelsStage $_.Name) -Force }

# Documents: short plain-text quick start + changelog + third-party notices.
Copy-Item (Join-Path $repoRoot "CHANGELOG.md") (Join-Path $stage "CHANGELOG.md") -Force
Copy-Item (Join-Path $repoRoot "THIRD-PARTY-NOTICES.md") (Join-Path $stage "THIRD-PARTY-NOTICES.md") -Force

$readme = @"
HaloCap $Version (native, win64)

Run HaloCap.exe. It lives in the notification area:
  Radial      F6
  Screenshot  F1
  OCR         F7
  Record      F8
  Pin         F3
  Settings    tray menu

In the screenshot selection press R to go back to the previous region; press it
again to walk further back (up to 10 entries). R is ignored while the selection
has annotations. Esc always quits the selection in one press.

The tray menu and the radial stay on screen while you start a selection: they are
captured into the frozen frame, so you can drag over them to include them in the
shot. They hand the mouse and keyboard over to the selection and the layering is
driven by focus, so clicking a pinned card always hits the card, never the menu or
the radial behind it.

Long clipboard text is pinned at full size: the card grows with the wrapped line
count instead of being cut off, and the pin is never shrunk, so a long document
just makes a long pin (drag or scroll to read the rest).

OCR models are in models\ppocrv6-tiny-1. The app also looks in the LOCALAPPDATA
folder under HaloCap2\models\ppocrv6-tiny-1 and honours HALOCAP_OCR_MODELS.
Settings and logs live in APPDATA under HaloCap2.
"@
Set-Content -Path (Join-Path $stage "README.txt") -Value $readme -Encoding utf8
Set-Content -Path (Join-Path $stage "version.txt") -Value "HaloCap $Version native win64" -Encoding ascii

$files = Get-ChildItem $stage -Recurse -File | ForEach-Object {
    [pscustomobject]@{
        path = $_.FullName.Substring($stage.Length + 1).Replace('\', '/')
        size = $_.Length
        sha256 = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower()
    }
}

$zip = Join-Path $dist "HaloCap-$Version-win-x64.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip -CompressionLevel Optimal

$zipInfo = Get-Item $zip
$zipHash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
$limit = 20 * 1024 * 1024

$manifest = [pscustomobject]@{
    version = $Version
    channel = "stable"
    url = "https://github.com/Joshua-Zion/HaloCap/releases/download/v$Version/HaloCap-$Version-win-x64.zip"
    sha256 = $zipHash
    size = $zipInfo.Length
    notes = "HaloCap $Version native win-x64 release"
    files = $files
}
$manifestPath = Join-Path $dist "HaloCap-$Version-win-x64.json"
$manifest | ConvertTo-Json -Depth 5 | Set-Content -Path $manifestPath -Encoding utf8

Write-Host "PACKAGE_STAGE=$stage"
Write-Host "PACKAGE_ZIP=$zip"
Write-Host "PACKAGE_BYTES=$($zipInfo.Length)"
Write-Host "PACKAGE_MB=$([math]::Round($zipInfo.Length / 1MB, 2))"
Write-Host "PACKAGE_UNDER_20MB=$(if ($zipInfo.Length -le $limit) { 1 } else { 0 })"
Write-Host "PACKAGE_SHA256=$zipHash"
Write-Host "PACKAGE_MANIFEST=$manifestPath"
Write-Host "PACKAGE_SIGNED=0"
Write-Host "PACKAGE_FILE_COUNT=$($files.Count)"
