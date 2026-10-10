param([Parameter(Mandatory = $true)][string]$BuildDirectory)

$ErrorActionPreference = 'Stop'
$build = (Resolve-Path -LiteralPath $BuildDirectory).Path
$binary = Join-Path $build 'tests\HoroCharacterInputTests.exe'
$evidence = Join-Path $build 'character-input-diagnostics'
New-Item -ItemType Directory -Force -Path $evidence | Out-Null
$status = Join-Path $evidence 'status.txt'
$case = 'HoroCharacterInputTests::Load-time allocation failure preserves caller grant and successful motion path does not allocate'

# Inspect only the installed x64 SDK debugger. Never fetch tools or modify PATH.
$kits = "${env:ProgramFiles(x86)}\Windows Kits\10"
$debugger = Join-Path $kits 'Debuggers\x64\cdb.exe'
$available = Test-Path -LiteralPath $debugger -PathType Leaf
if ($available) {
  $signature = Get-AuthenticodeSignature -LiteralPath $debugger
  $available = $signature.Status -eq 'Valid' -and $signature.SignerCertificate.Subject -match 'O=Microsoft Corporation'
}
if ($available) {
  $version = (Get-Item -LiteralPath $debugger).VersionInfo.FileVersion
  $hash = (Get-FileHash -LiteralPath $debugger -Algorithm SHA256).Hash
  "Installed Microsoft SDK CDB: version=$version sha256=$hash" | Set-Content -LiteralPath $status
} else {
  'STACK EVIDENCE UNAVAILABLE: installed signed Microsoft x64 SDK CDB absent; no download attempted.' |
    Set-Content -LiteralPath $status
}

# Preserve the real CTest failure and 90s test deadline. The normal suite still runs separately.
$start = [System.Diagnostics.ProcessStartInfo]::new('ctest')
$start.UseShellExecute = $false
foreach ($argument in @('--test-dir', $build, '--parallel', '1', '--timeout', '90', '--no-tests=error',
                        '--output-on-failure', '--output-junit', (Join-Path $evidence 'ctest.xml'),
                        '-R', ('^' + [regex]::Escape($case) + '$'))) {
  $start.ArgumentList.Add($argument)
}
$ctest = [System.Diagnostics.Process]::Start($start)
$observed = $null
$birth = $null
$attempted = $false
try {
  while (-not $ctest.HasExited) {
    if ($available -and -not $attempted) {
      try {
      $children = @(Get-CimInstance Win32_Process -Filter "ParentProcessId = $($ctest.Id)" |
        Where-Object { $_.ExecutablePath -eq $binary -and $_.CreationDate -ge $ctest.StartTime })
      if ($children.Count -eq 1) {
        $child = $children[0]
        if ($null -eq $observed) {
          $observed = [System.Diagnostics.Process]::GetProcessById($child.ProcessId)
          $birth = $child.CreationDate
        }
        # PID, start time, direct parent and executable must all still match; never attach by name.
        if ($child.ProcessId -eq $observed.Id -and -not $observed.HasExited -and
            $child.CreationDate -eq $birth -and
            [Math]::Abs(($child.CreationDate - $observed.StartTime).TotalMilliseconds) -le 1 -and
            ([DateTime]::Now - $observed.StartTime).TotalSeconds -ge 30) {
          $attempted = $true
          "Owned test PID=$($observed.Id); elapsed >=30s; one bounded stack-only attempt." |
            Add-Content -LiteralPath $status
          $reader = [System.Diagnostics.ProcessStartInfo]::new($debugger)
          $reader.UseShellExecute = $false
          # Noninvasive/non-suspending; local PDBs only; frame symbols, not arguments, memory or environment.
          foreach ($argument in @('-pvr', '-p', "$($observed.Id)", '-netsyms:no', '-y', (Join-Path $build 'tests'),
                                  '-logo', (Join-Path $evidence 'threads.txt'), '-c', '~*kn 64;qd')) {
            $reader.ArgumentList.Add($argument)
          }
          $capture = [System.Diagnostics.Process]::Start($reader)
          try {
            if (-not $capture.WaitForExit(10000)) {
              # Stop only this debugger process, never the test or any unrelated process.
              $capture.Kill()
              if (-not $capture.WaitForExit(1000)) {
                'STACK EVIDENCE INCOMPLETE: owned reader cleanup did not finish within 1s.' |
                  Add-Content -LiteralPath $status
              }
              'STACK EVIDENCE INCOMPLETE: CDB exceeded 10s bound.' | Add-Content -LiteralPath $status
            } else {
              "CDB exit=$($capture.ExitCode); inspect threads.txt for actual symbol/stack evidence." |
                Add-Content -LiteralPath $status
            }
          } finally {
            $capture.Dispose()
          }
        }
      }
      } catch {
        # Exit races or unavailable diagnostics must not replace the real test result.
        $attempted = $true
        'STACK EVIDENCE UNAVAILABLE: process observation or reader failed; CTest continues unchanged.' |
          Add-Content -LiteralPath $status
      }
    }
    Start-Sleep -Milliseconds 200
  }
  $ctest.WaitForExit()
  $result = $ctest.ExitCode
  "CTest exit=$result; captureAttempted=$attempted; original 90s test deadline unchanged." |
    Add-Content -LiteralPath $status
} finally {
  if ($null -ne $observed) { $observed.Dispose() }
  $ctest.Dispose()
}
Get-Content -LiteralPath $status
exit $result
