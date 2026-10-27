param([Parameter(Mandatory)][string]$Profiler,[Parameter(Mandatory)][string]$Fixture)
$ErrorActionPreference='Stop'
$testBase=[IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$testDir=Join-Path $testBase ('mksm-sample-'+[Guid]::NewGuid().ToString('N'))
$testProcess=$null
try {
    New-Item -ItemType Directory -Path $testDir | Out-Null
    $testProcess=Start-Process -FilePath (Resolve-Path -LiteralPath $Fixture).Path -WindowStyle Hidden -PassThru
    $testOut=Join-Path $testDir 'out.log';$testErr=Join-Path $testDir 'err.log'
    & $Profiler $testProcess.Id 2 2 20 > $testOut 2> $testErr
    if($LASTEXITCODE -ne 0){throw 'The profiler failed'}
    $testText=Get-Content -LiteralPath $testOut -Raw
    $testError=Get-Content -LiteralPath $testErr -Raw
    if($testError){throw ('DbgHelp error: '+$testError)}
    if($testText -notmatch 'executable frames from DLL:'){throw 'The recovered stacks are not shown'}
    $testSection=($testText -split 'executable frames from DLL:',2)[1]
    if($testSection -notmatch 'EsperaPerfilProcedural'){throw 'The procedural caller was not recovered'}
    Write-Output 'Profiler: one DbgHelp session and own caller recovered from DLL: OK'
}
finally {
    if($null -ne $testProcess -and -not $testProcess.HasExited){Stop-Process -Id $testProcess.Id -Force}
    $testResolved=[IO.Path]::GetFullPath($testDir)
    $testPrefix=$testBase.TrimEnd([IO.Path]::DirectorySeparatorChar,[IO.Path]::AltDirectorySeparatorChar)+[IO.Path]::DirectorySeparatorChar
    if(-not $testResolved.StartsWith($testPrefix,[StringComparison]::OrdinalIgnoreCase) -or
       (Split-Path -Leaf $testResolved) -notmatch '^mksm-sample-[a-f0-9]{32}$'){throw 'Invalid cleanup target'}
    if(Test-Path -LiteralPath $testResolved){Remove-Item -LiteralPath $testResolved -Recurse -Force}
}
