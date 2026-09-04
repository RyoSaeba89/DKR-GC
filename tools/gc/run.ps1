param(
  [string]$Tag = "run",
  [int]$Seconds = 150,
  [int]$ShotEvery = 10,
  [int]$ShotAfter = 15,
  [string]$Presses = "",          # "28,34" seconds at which to press START, or "28:ENTER,40:X" for other keys
  [string]$Dol = "C:\Users\jacqu\Documents\DKR-GC\dkr\build\gc\dkr.dol"
)

$SP = "C:\Users\jacqu\Documents\DKR-GC\dkr\build\gc\capture"
$Out = Join-Path $SP "$Tag.log"
$ShotDir = Join-Path $SP "shots"

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W4 {
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  public struct RECT { public int Left, Top, Right, Bottom; }
}
"@

New-Item -ItemType Directory -Force -Path $ShotDir | Out-Null
Get-Process Dolphin -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 800
Remove-Item $Out -ErrorAction SilentlyContinue

$exe = "C:\Users\jacqu\Documents\GCRadio\Dolphin-x64\Dolphin.exe"
$proc = Start-Process -FilePath $exe -ArgumentList @("-b", "-e", $Dol) -PassThru

$client = $null
$deadline = (Get-Date).AddSeconds(20)
while ((Get-Date) -lt $deadline -and $null -eq $client) {
  try { $c = New-Object System.Net.Sockets.TcpClient; $c.Connect("127.0.0.1", 0xd6ec); $client = $c }
  catch { Start-Sleep -Milliseconds 300 }
}
if ($null -eq $client) { Write-Output "NO GECKO SOCKET"; exit 1 }

$stream = $client.GetStream(); $stream.ReadTimeout = 300
$sw = New-Object System.IO.StreamWriter($Out, $false)
$buf = New-Object byte[] 16384
$wsh = New-Object -ComObject WScript.Shell
$start = Get-Date
$end = $start.AddSeconds($Seconds)

$vkMap = @{ "ENTER" = 0x0D; "X" = 0x58; "Z" = 0x5A; "UP" = 0x26; "DOWN" = 0x28; "LEFT" = 0x25; "RIGHT" = 0x27 }
$pressList = @()
if ($Presses -ne "") {
  $pressList = $Presses.Split(",") | ForEach-Object {
    $bits = $_.Split(":")
    if ($bits.Count -gt 1) { @{ t = [double]$bits[0]; k = $bits[1].ToUpper() } }
    else { @{ t = [double]$bits[0]; k = "ENTER" } }
  }
}
$pressDone = @($false) * ($pressList.Count + 1)
$nextShot = $ShotAfter
$exitNoted = $false
$shotIdx = 0

function Get-Shot($proc, $path) {
  $proc.Refresh()
  $h = $proc.MainWindowHandle
  if ($h -eq [IntPtr]::Zero) { return $null }
  [void][W4]::SetWindowPos($h, [IntPtr](-1), -1900, 40, 1000, 800, 0x0040)
  Start-Sleep -Milliseconds 400
  $r = New-Object "W4+RECT"
  [void][W4]::GetWindowRect($h, [ref]$r)
  $bmp = New-Object System.Drawing.Bitmap ($r.Right - $r.Left), ($r.Bottom - $r.Top)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
  $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
  $g.Dispose(); $bmp.Dispose()
  return $path
}

while ((Get-Date) -lt $end) {
  try { $n = $stream.Read($buf, 0, $buf.Length)
        if ($n -gt 0) { $sw.Write([System.Text.Encoding]::ASCII.GetString($buf, 0, $n)); $sw.Flush() } } catch { }
  $el = ((Get-Date) - $start).TotalSeconds
  if ($proc.HasExited -and -not $exitNoted) {
    $sw.Write("`n#### DOLPHIN EXITED at t=$([int]$el)s code $($proc.ExitCode)`n"); $sw.Flush()
    $exitNoted = $true
  }

  for ($i = 0; $i -lt $pressList.Count; $i++) {
    if (-not $pressDone[$i] -and $el -gt $pressList[$i].t) {
      $vk = [byte]$vkMap[$pressList[$i].k]
      [void]$wsh.AppActivate($proc.Id)
      Start-Sleep -Milliseconds 200
      [W4]::keybd_event($vk, 0, 0, [UIntPtr]::Zero)
      Start-Sleep -Milliseconds 80
      [W4]::keybd_event($vk, 0, 2, [UIntPtr]::Zero)
      $pressDone[$i] = $true
      $sw.Write("`n#### PRESS $($pressList[$i].k) at t=$([int]$el)s`n"); $sw.Flush()
    }
  }

  if ($ShotEvery -gt 0 -and $el -gt $nextShot) {
    $p = Join-Path $ShotDir ("{0}-{1:00}-t{2:000}.png" -f $Tag, $shotIdx, [int]$el)
    $r = Get-Shot $proc $p
    if ($r) { Write-Output $r }
    $sw.Write("`n#### SHOT $shotIdx at t=$([int]$el)s`n"); $sw.Flush()
    $shotIdx++
    $nextShot += $ShotEvery
  }
}

$sw.Close(); $client.Close()
Get-Process Dolphin -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Output "captured $((Get-Item $Out).Length) bytes -> $Out"
