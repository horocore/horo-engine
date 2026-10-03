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
    Write-Output "Staged symbol library: $source ($((Get-Item -LiteralPath $source).VersionInfo.FileVersion))"
  }
}

# Recent DbgHelp readers load DIA dynamically when opening PDBs. Stage its x64
# reader as well; copying only DbgHelp's static companions does not prove PDB support.
$dia = Join-Path $reader.DirectoryName 'msdia140.dll'
if (-not (Test-Path -LiteralPath $dia -PathType Leaf)) {
  if (-not $env:VSINSTALLDIR) { throw 'Visual Studio installation path is unavailable for the x64 DIA reader' }
  $dia = Join-Path $env:VSINSTALLDIR 'DIA SDK\bin\amd64\msdia140.dll'
}
if (-not (Test-Path -LiteralPath $dia -PathType Leaf)) { throw "Windows x64 DIA symbol reader is unavailable: $dia" }
Copy-Item -LiteralPath $dia -Destination (Join-Path $destination 'msdia140.dll') -Force
Write-Output "Staged DIA symbol library: $dia ($((Get-Item -LiteralPath $dia).VersionInfo.FileVersion))"

# Run from outside the binary directory to catch accidental reliance on the caller's working directory.
Push-Location ([System.IO.Path]::GetTempPath())
try {
  & (Join-Path $destination 'HoroMixerAllocationFailureWorker.exe') --check-symbols
  if ($LASTEXITCODE -ne 0) { throw "Allocation worker PDB self-check failed: $LASTEXITCODE" }
} finally {
  Pop-Location
}
