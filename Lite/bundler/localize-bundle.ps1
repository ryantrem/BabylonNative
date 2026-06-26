<#
.SYNOPSIS
  Localize a freshly-built Lite bundle for a self-contained demo: repoint every asset URL
  (and __LITE_PUBLIC_ROOT) at a local assets directory, by basename.

.DESCRIPTION
  The bundler (build-lite.mjs) bakes absolute dev-machine asset paths into each bundle
  (e.g. file:///D:/Repos/.../cubes.glb, https://assets.babylonjs.com/.../env). A shippable
  demo needs those pointing at files next to the exe instead. This script rewrites every
  http(s):// or file:// URL that ends in a known asset extension to
  file:///<AssetDir>/<basename>, and sets __LITE_PUBLIC_ROOT to file:///<AssetDir>.

  It does NOT minify — run `node build-lite.mjs` first (minification, including the prelude
  banner, happens there). This is purely the deploy-time URL rewrite.

.PARAMETER Bundle
  Path to the freshly built dist bundle (e.g. dist/cubesbundle.lite.js).

.PARAMETER OutFile
  Destination path for the localized bundle (e.g. <demo>/cubes.lite.js).

.PARAMETER AssetDir
  Directory (next to the exe) that holds the demo's assets. Asset URLs are repointed here
  by basename. Must already contain the referenced files.

.EXAMPLE
  ./localize-bundle.ps1 -Bundle dist/cubesbundle.lite.js `
      -OutFile "C:/.../cubes/lite/v8/cubes.lite.js" `
      -AssetDir "C:/.../cubes/lite/v8/assets"
#>
param(
    [Parameter(Mandatory)] [string]$Bundle,
    [Parameter(Mandatory)] [string]$OutFile,
    [Parameter(Mandatory)] [string]$AssetDir
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Bundle)) { throw "Bundle not found: $Bundle" }
if (-not (Test-Path $AssetDir)) { throw "AssetDir not found: $AssetDir" }

# Normalize the asset dir to a forward-slash file:// base (no trailing slash).
$assetBase = "file:///" + ((Resolve-Path $AssetDir).Path -replace '\\', '/').TrimEnd('/')

$raw = Get-Content $Bundle -Raw

# Repoint every quoted asset URL to <assetBase>/<basename>. Covers http(s):// and file://
# URLs ending in a known asset extension.
$assetExt = 'glb|gltf|bin|env|dds|png|jpg|jpeg|ktx|ktx2|hdr|basis'
$raw = [regex]::Replace($raw, "(?<q>[`"'])(?:https?://|file://)[^`"']+\.(?:$assetExt)\k<q>", {
    param($m)
    $q = $m.Groups['q'].Value
    $url = $m.Value.Trim($q)
    $base = [System.IO.Path]::GetFileName(($url -split '\?')[0])
    return "$q$assetBase/$base$q"
})

# Point the root-relative asset resolver (prelude fetch shim) at the local assets dir too.
$raw = [regex]::Replace($raw,
    '__LITE_PUBLIC_ROOT\s*=\s*"(?:file://|https?://)[^"]*"',
    "__LITE_PUBLIC_ROOT=`"$assetBase`"")

$outDir = Split-Path -Parent $OutFile
if ($outDir -and -not (Test-Path $outDir)) { New-Item -ItemType Directory -Force -Path $outDir | Out-Null }
Set-Content -Path $OutFile -Value $raw -NoNewline -Encoding utf8

# Report: confirm the rewrite + that the referenced assets exist locally.
Write-Host "localized -> $OutFile"
Write-Host "  asset base: $assetBase"
$refs = [regex]::Matches($raw, '(?:file://)[^"'']+\.(?:' + $assetExt + ')') | ForEach-Object { $_.Value } | Select-Object -Unique
foreach ($r in $refs) {
    $name = [System.IO.Path]::GetFileName($r)
    $exists = Test-Path (Join-Path $AssetDir $name)
    Write-Host ("  {0}  {1}" -f ($(if ($exists) { "OK " } else { "MISSING" }), $name))
}
