param([Parameter(Mandatory = $true)][string]$BinaryDirectory)

$ErrorActionPreference = 'Stop'
$kits = "${env:ProgramFiles(x86)}\Windows Kits\10"
$debuggerReader = Join-Path $kits 'Debuggers\x64\dbghelp.dll'
if (Test-Path -LiteralPath $debuggerReader -PathType Leaf) {
  $reader = Get-Item -LiteralPath $debuggerReader
} else {
  $reader = Get-ChildItem "$kits\bin\*\x64\dbghelp.dll" -ErrorAction SilentlyContinue |
    Sort-Object { $_.VersionInfo.FileVersionRaw } -Descending | Select-Object -First 1
}
if (-not $reader) { throw 'Windows SDK x64 symbol reader is unavailable' }
$destination = (Resolve-Path -LiteralPath $BinaryDirectory).Path
foreach ($artifact in @('HoroMixerAllocationFailureWorker.exe', 'HoroMixerAllocationFailureWorker.pdb')) {
  $path = Join-Path $destination $artifact
  if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item -LiteralPath $path).Length -eq 0) {
    throw "Allocation diagnostic artifact is missing or empty: $path"
  }
}
# Keep package-local dependencies together; do not modify the system DLL or PATH.
foreach ($library in @('dbghelp.dll', 'dbgcore.dll', 'symsrv.dll', 'srcsrv.dll')) {
  $source = Join-Path $reader.DirectoryName $library
  if (Test-Path -LiteralPath $source -PathType Leaf) {
    Copy-Item -LiteralPath $source -Destination (Join-Path $destination $library) -Force
    Write-Host "Staged symbol library: $source ($((Get-Item -LiteralPath $source).VersionInfo.FileVersion))"
  }
}