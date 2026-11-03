# Shared settings of the scripts: the pinned PS2Recomp commit, the folders and where the game files are.
$RepoRoot = Split-Path -Parent $PSScriptRoot
$PS2RecompDir = Join-Path $RepoRoot 'PS2Recomp'
$PS2RecompCommit = 'e563698'   # ran-j/PS2Recomp, "GIF: treat FLG=3 like IMAGE everywhere a GIFtag is parsed (#384)"
$LogsDir = Join-Path $RepoRoot 'logs'
$GameDir = Join-Path $RepoRoot 'game'
$GameElfName = 'SLES_535.24'   # Mortal Kombat: Shaolin Monks, PAL (SLES-53524)

# The game ELF: $env:MKSM_ELF if set, otherwise game\SLES_535.24 (the disc extracted into game\, see the README).
function Get-GameElf {
    if ($env:MKSM_ELF) { return (Resolve-Path -LiteralPath $env:MKSM_ELF).Path }
    $elf = Join-Path $GameDir $GameElfName
    if (-not (Test-Path -LiteralPath $elf)) {
        throw "The game ELF was not found at $elf. Extract the PAL disc into game\ (see the README) or set MKSM_ELF."
    }
    return $elf
}
New-Item -ItemType Directory -Force -Path $LogsDir | Out-Null
