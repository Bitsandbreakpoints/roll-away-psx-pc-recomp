param([string]$dir, [string]$frames, [string]$out)
# A montage (320x240 each, four per row, frame numbers in the corner) of
# the pictures shot_<frame>.bmp that AP_VRAM_DUMP wrote in $dir.
Add-Type -AssemblyName System.Drawing
$list = $frames.Split(',')
$cols = [Math]::Min(4, $list.Count)
$rows = [Math]::Ceiling($list.Count / $cols)
$m = New-Object System.Drawing.Bitmap (320 * $cols), (240 * $rows)
$g = [System.Drawing.Graphics]::FromImage($m)
$font = New-Object System.Drawing.Font 'Arial', 10
$i = 0
foreach ($f in $list) {
  $x = ($i % $cols) * 320
  $y = [Math]::Floor($i / $cols) * 240
  $file = Join-Path $dir "shot_$f.bmp"
  if (Test-Path $file) {
    $b = [System.Drawing.Bitmap]::FromFile($file)
    $g.DrawImage($b, $x, $y, 320, 240)
    $b.Dispose()
  }
  $g.DrawString($f, $font, [System.Drawing.Brushes]::Yellow, $x + 3, $y + 3)
  $i++
}
$m.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
