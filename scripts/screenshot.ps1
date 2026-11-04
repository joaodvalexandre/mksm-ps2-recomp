# Saves a PNG of the game window (the runner's window title starts with "PS2-Recomp") so a run can be checked
# without watching it: scripts\screenshot.ps1 [-Out logs\shot.png]
# The window is rendered with PrintWindow, so it is captured even when another window covers it; nothing else
# on the screen ends up in the image.
param([string]$Out = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')
if (-not $Out) { $Out = Join-Path $LogsDir ('shot-' + (Get-Date -Format 'HHmmss') + '.png') }

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class MksmWin {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
}
"@
$process = Get-Process | Where-Object { $_.MainWindowTitle -like 'PS2-Recomp*' } | Select-Object -First 1
if (-not $process) { throw 'No window with a title starting with "PS2-Recomp" is open.' }
$handle = $process.MainWindowHandle
$rect = New-Object MksmWin+RECT
[MksmWin]::GetClientRect($handle, [ref]$rect) | Out-Null
$width = $rect.Right - $rect.Left
$height = $rect.Bottom - $rect.Top
if ($width -le 0 -or $height -le 0) { throw "The window has no client area ($width x $height); is it minimized?" }
$bitmap = New-Object System.Drawing.Bitmap($width, $height)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$hdc = $graphics.GetHdc()
# PW_CLIENTONLY (1) | PW_RENDERFULLCONTENT (2): the client area as composed by the desktop window manager.
$rendered = [MksmWin]::PrintWindow($handle, $hdc, 3)
$graphics.ReleaseHdc($hdc)
if (-not $rendered) { throw 'PrintWindow failed.' }
$bitmap.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$graphics.Dispose(); $bitmap.Dispose()
Write-Host "Saved $Out ($width x $height, window '$($process.MainWindowTitle)')"
