param(
  [string]$BuildDirectory = (Join-Path $PSScriptRoot '..\build-msvc-Release'),
  [string]$OutputPath = ''
)
$ErrorActionPreference = 'Stop'
$repository = Split-Path $PSScriptRoot -Parent
$releaseModel = Join-Path $repository 'models\language_model.bin'
$builtModel = Join-Path $BuildDirectory 'language_model.bin'
if ((Get-FileHash -LiteralPath $releaseModel).Hash -ne (Get-FileHash -LiteralPath $builtModel).Hash) {
  throw 'Build model differs from the reviewed release model. Rebuild before packaging.'
}
if (-not $OutputPath) {
  $stamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')
  $OutputPath = Join-Path $BuildDirectory "DiSwitcher-local-ai-$stamp.zip"
}
# Explicit allowlist. Never archive the whole build folder or LocalAppData.
$entries = [ordered]@{
  'Diswitcher.exe' = (Join-Path $BuildDirectory 'Diswitcher.exe')
  'language_model.bin' = $builtModel
  'model-report.json' = (Join-Path $repository 'models\language_model.report.json')
  'README.md' = (Join-Path $repository 'README.md')
}
Get-ChildItem -LiteralPath (Join-Path $repository 'models\licenses') -File | ForEach-Object {
  $entries["model-licenses/$($_.Name)"] = $_.FullName
}
foreach ($file in $entries.Values) {
  if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing package file: $file" }
}
$report = Get-Content -LiteralPath $entries['model-report.json'] -Raw | ConvertFrom-Json
if ($report.sha256 -ne (Get-FileHash -LiteralPath $releaseModel).Hash) {
  throw 'Model report does not match the release model.'
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::Open([IO.Path]::GetFullPath($OutputPath),[IO.Compression.ZipArchiveMode]::Create)
try {
  foreach ($entry in $entries.GetEnumerator()) {
    [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive,$entry.Value,$entry.Key,[IO.Compression.CompressionLevel]::Optimal) | Out-Null
  }
} finally { $archive.Dispose() }
Write-Host "Packaged: $OutputPath"
