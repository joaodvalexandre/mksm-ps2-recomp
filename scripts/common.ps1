# Paths shared by the build and run scripts.
# Loaded with: . (Join-Path $PSScriptRoot 'common.ps1')

$RepoRoot = Split-Path -Parent $PSScriptRoot
$LogsDir  = Join-Path $RepoRoot 'logs'
New-Item -ItemType Directory -Force -Path $LogsDir | Out-Null

# PS2Recomp commit on which the patches in patches/ are applied
$PS2RecompUrl    = 'https://github.com/ran-j/PS2Recomp.git'
$PS2RecompCommit = 'c5a9d02573410a2085a4b4b831b0b68ba3515440'

# Work folder (PS2Recomp clone and generated code): <repository>\mksm-work. It lives inside the repository folder
# (never in the root of a drive) and is git-ignored, so it is never pushed. Inside a git worktree
# (<repo>\.claude\worktrees\<name>) it is still the folder of the main checkout.
# It can be changed with the MKSM_WORK environment variable (keep the path short: Windows has a 260-character limit).
function Get-MksmWorkDir {
    if ($env:MKSM_WORK) { return $env:MKSM_WORK }
    $main = $RepoRoot
    $marker = [IO.Path]::DirectorySeparatorChar + '.claude' + [IO.Path]::DirectorySeparatorChar + 'worktrees' + [IO.Path]::DirectorySeparatorChar
    $at = $RepoRoot.IndexOf($marker, [StringComparison]::OrdinalIgnoreCase)
    if ($at -ge 0) { $main = $RepoRoot.Substring(0, $at) }
    return Join-Path $main 'mksm-work'
}

# Game executable. Search order:
#   1. the MKSM_ELF environment variable
#   2. game\SLES_535.24 (Mortal Kombat: Shaolin Monks, PAL) inside the repository
function Get-MksmElf {
    $candidates = @()
    if ($env:MKSM_ELF) { $candidates += $env:MKSM_ELF }
    $candidates += (Join-Path $RepoRoot 'game\SLES_535.24')
    foreach ($c in $candidates) {
        if (Test-Path -LiteralPath $c) { return (Resolve-Path -LiteralPath $c).Path }
    }
    throw "The game executable was not found. Copy it into game\ or set MKSM_ELF. Searched in:`n  $($candidates -join "`n  ")"
}
