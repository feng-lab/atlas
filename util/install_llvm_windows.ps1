param(
  [string]$LlvmVersion = '22.1.2',
  [string]$LlvmWin64TarXzSha256 = '6550bcc308bf972f7f956001b73f6478da3d22f1ebd4b01653c978a6c7ff3698'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function TryGet-InstalledLlvmVersion {
  param(
    [Parameter(Mandatory = $true)]
    [string]$ClangClPath
  )

  $output = (& $ClangClPath --version 2>&1 | Out-String)
  if ($LASTEXITCODE -ne 0) {
    Write-Warning "Failed to query LLVM version from $ClangClPath; replacing existing install. Output: $output"
    return $null
  }
  $match = [regex]::Match($output, 'clang version\s+([0-9]+(?:\.[0-9]+){1,3})')
  if (-not $match.Success) {
    Write-Warning "Could not parse LLVM version from $ClangClPath output; replacing existing install. Output: $output"
    return $null
  }
  return $match.Groups[1].Value
}

$ver = $LlvmVersion
if ([string]::IsNullOrWhiteSpace($ver)) {
  Write-Error 'LlvmVersion is not set.'
}

$expectedSha256 = $LlvmWin64TarXzSha256
if ([string]::IsNullOrWhiteSpace($expectedSha256)) {
  Write-Error 'LlvmWin64TarXzSha256 is not set.'
}

$sevenZip = Join-Path $PSScriptRoot '7za.exe'
if (-not (Test-Path $sevenZip)) {
  Write-Error "7za.exe not found at $sevenZip"
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$llvmRoot = Join-Path $repoRoot 'llvm'
$llvmRoot = [System.IO.Path]::GetFullPath($llvmRoot)

Write-Host "Using LLVM install root: $llvmRoot"

$archiveName = "clang+llvm-$ver-x86_64-pc-windows-msvc.tar.xz"
$downloadRoot = Join-Path $repoRoot 'llvm.__download'
$archivePath = Join-Path $downloadRoot $archiveName
$url = "https://github.com/llvm/llvm-project/releases/download/llvmorg-$ver/$archiveName"
$existingClangCl = Join-Path $llvmRoot 'bin\clang-cl.exe'
$existingLldLink = Join-Path $llvmRoot 'bin\lld-link.exe'

if (Test-Path $llvmRoot) {
  $replaceReason = $null
  if ((Test-Path $existingClangCl) -and (Test-Path $existingLldLink)) {
    $installedVersion = TryGet-InstalledLlvmVersion -ClangClPath $existingClangCl
    if ($installedVersion -and ($installedVersion -eq $ver)) {
      Write-Host "Reusing existing LLVM installation at $llvmRoot"
      & $existingClangCl --version
      return
    }
    if ($installedVersion) {
      $replaceReason = "installed version is $installedVersion, expected $ver"
    } else {
      $replaceReason = 'installed version could not be verified'
    }
  } else {
    $replaceReason = 'existing directory does not look like a complete LLVM installation'
  }
  Write-Host "Removing existing LLVM installation at $llvmRoot before reinstall because $replaceReason"
  Remove-Item -LiteralPath $llvmRoot -Recurse -Force
}

$downloadArchive = $true
New-Item -ItemType Directory -Force -Path $downloadRoot | Out-Null
if (Test-Path $archivePath) {
  $existingHash = (Get-FileHash -Algorithm SHA256 -Path $archivePath).Hash.ToLowerInvariant()
  if ($existingHash -eq $expectedSha256.ToLowerInvariant()) {
    Write-Host "Reusing existing LLVM archive at $archivePath"
    $downloadArchive = $false
  } else {
    Write-Host "Removing stale LLVM archive at $archivePath (SHA-256 mismatch)"
    Remove-Item -LiteralPath $archivePath -Force
  }
}

if ($downloadArchive) {
  Write-Host "Downloading LLVM $ver from: $url"
  Invoke-WebRequest -Uri $url -OutFile $archivePath -UseBasicParsing
}

if (-not (Test-Path $archivePath)) {
  Write-Error "Download failed: $archivePath not found"
}

$hash = (Get-FileHash -Algorithm SHA256 -Path $archivePath).Hash.ToLowerInvariant()
if ($hash -ne $expectedSha256.ToLowerInvariant()) {
  Write-Error "LLVM archive SHA-256 mismatch. Expected $expectedSha256, got $hash"
}

$stageBase = "$llvmRoot.__extract"
$xzStage = Join-Path $stageBase 'xz'
$tarStage = Join-Path $stageBase 'tar'
$llvmRootParent = Split-Path -Parent $llvmRoot
if ([string]::IsNullOrWhiteSpace($llvmRootParent) -or -not (Test-Path $llvmRootParent)) {
  Write-Error "LLVM install parent directory does not exist: $llvmRootParent"
}

if (Test-Path $stageBase) {
  Write-Host "Removing stale LLVM staging directory at $stageBase"
  Remove-Item -LiteralPath $stageBase -Recurse -Force
}

New-Item -ItemType Directory -Force -Path $xzStage | Out-Null
New-Item -ItemType Directory -Force -Path $tarStage | Out-Null
New-Item -ItemType Directory -Force -Path $llvmRoot | Out-Null

Write-Host "Extracting LLVM $ver with $sevenZip into $llvmRoot"
& $sevenZip x $archivePath "-o$xzStage" -y -bso1 -bsp1
if ($LASTEXITCODE -ne 0) {
  Write-Error "LLVM xz extraction failed with exit code $LASTEXITCODE"
}

$tarPath = Join-Path $xzStage ([System.IO.Path]::GetFileNameWithoutExtension($archiveName))
if (-not (Test-Path $tarPath)) {
  Write-Error "LLVM tar payload not found after xz extraction: $tarPath"
}

& $sevenZip x $tarPath "-o$tarStage" -y -bso1 -bsp1
if ($LASTEXITCODE -ne 0) {
  Write-Error "LLVM tar extraction failed with exit code $LASTEXITCODE"
}

$topLevelEntries = @(Get-ChildItem -LiteralPath $tarStage -Force)
if ($topLevelEntries.Count -ne 1 -or -not $topLevelEntries[0].PSIsContainer) {
  $entryNames = ($topLevelEntries | ForEach-Object { $_.FullName }) -join '; '
  Write-Error "Expected exactly one top-level directory in LLVM archive, found $($topLevelEntries.Count): $entryNames"
}

$sourceRoot = $topLevelEntries[0].FullName
foreach ($entry in Get-ChildItem -LiteralPath $sourceRoot -Force) {
  Move-Item -LiteralPath $entry.FullName -Destination $llvmRoot -Force
}

Remove-Item -LiteralPath $stageBase -Recurse -Force

$clangCl = Join-Path $llvmRoot 'bin\clang-cl.exe'
$lldLink = Join-Path $llvmRoot 'bin\lld-link.exe'
if (-not (Test-Path $clangCl)) {
  Write-Error "clang-cl.exe not found under $llvmRoot\bin"
}
if (-not (Test-Path $lldLink)) {
  Write-Error "lld-link.exe not found under $llvmRoot\bin"
}

& $clangCl --version
