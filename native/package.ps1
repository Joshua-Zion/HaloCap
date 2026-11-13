# S5 delivery packaging: stage the native build, hash it, zip it, and write the
# update manifest next to the zip. Signing is intentionally skipped here: the
# 2.0 branch does not sign or publish (see docs/2.0-native-plan.md section 10);
# the release line on main signs with signtool + the project certificate.
param(
    [string]$Version = "2.0.0",
    [string]$Output = "dist"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$appDir = Join-Path $root "build\app"
$exe = Join-Path $appDir "HaloCap.exe"
$ort = Join-Path $appDir "onnxruntime.dll"
$models = Join-Path $root "models\ppocrv6-tiny-1"

foreach ($required in @($exe, $ort, (Join-Path $models "ppocrv6_tiny_det.onnx"))) {
    if (-not (Test-Path $required)) {
        throw "missing build input: $required (run build.cmd first)"
    }
}

$dist = Join-Path $root $Output
$stage = Join-Path $dist "HaloCap-$Version-win64"
$modelsStage = Join-Path $stage "models\ppocrv6-tiny-1"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $modelsStage -Force | Out-Null

Copy-Item $exe (Join-Path $stage "HaloCap.exe") -Force
Copy-Item $ort (Join-Path $stage "onnxruntime.dll") -Force
Get-ChildItem $models -File | Where-Object { $_.Extension -in @(".onnx", ".txt") } |
    ForEach-Object { Copy-Item $_.FullName (Join-Path $modelsStage $_.Name) -Force }

$readme = @"
HaloCap $Version (native, win64)

Run HaloCap.exe. It lives in the notification area:
  Screenshot  Ctrl+Alt+A
  OCR         Ctrl+Alt+O
  Record      Ctrl+Alt+R
  Pin         Ctrl+Alt+P
  Settings    tray menu

The OCR models are in models\ppocrv6-tiny-1. The app also looks in
%LOCALAPPDATA%\HaloCap2\models\ppocrv6-tiny-1 and honours HALOCAP_OCR_MODELS.
Settings and logs: %APPDATA%\HaloCap2.

This is a 2.0 branch candidate package, not a signed release.
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

$zip = Join-Path $dist "HaloCap-$Version-win64.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip -CompressionLevel Optimal

$zipInfo = Get-Item $zip
$zipHash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
$limit = 20 * 1024 * 1024

$manifest = [pscustomobject]@{
    version = $Version
    channel = "stable"
    url = "https://github.com/Joshua-Zion/HaloCap/releases/download/v$Version/HaloCap-$Version-win64.zip"
    sha256 = $zipHash
    size = $zipInfo.Length
    notes = "HaloCap $Version native win64 candidate package"
    files = $files
}
$manifestPath = Join-Path $dist "HaloCap-$Version-win64.json"
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
