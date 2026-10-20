param([string]$pairs, [string]$out)
# Compares emulator screenshots with the port's pictures.
#   -pairs "name|emulator.png|port.bmp;..."
# DuckStation shows the 224 lines inside the NTSC overscan stretched to
# 240, so the port's lines 8..231 are scaled the same way (a larger
# emulator picture, with the overscan, is searched for the best 320x240
# window instead).  Prints the mean absolute difference per channel
# (0-255) and the share of pixels differing by more than 24; writes a
# montage: emulator | port | difference x4.
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing @"
using System; using System.Drawing; using System.Drawing.Imaging;
public class Compare {
  public static Bitmap Scale(Bitmap b, Rectangle src) {
    var r = new Bitmap(320, 240, PixelFormat.Format24bppRgb);
    using (var g = Graphics.FromImage(r)) {
      g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBilinear;
      g.PixelOffsetMode = System.Drawing.Drawing2D.PixelOffsetMode.Half;
      g.DrawImage(b, new Rectangle(0, 0, 320, 240), src, GraphicsUnit.Pixel); }
    return r; }
  public static Bitmap Port(Bitmap b) {
    int f = b.Width / 320;
    return Scale(b, new Rectangle(0, 8 * f, b.Width, 224 * f)); }
  public static Bitmap Emulator(Bitmap b, Bitmap port) {
    if (b.Width <= 320 || b.Height <= 240) return Scale(b, new Rectangle(0, 0, b.Width, b.Height));
    int bx = 0, by = 0; long best = long.MaxValue;
    for (int oy = 0; oy + 240 <= b.Height; oy++) for (int ox = 0; ox + 320 <= b.Width; ox++) {
      long sum = 0;
      for (int y = 0; y < 240 && sum < best; y += 4) for (int x = 0; x < 320; x += 4) {
        Color p = b.GetPixel(ox + x, oy + y), q = port.GetPixel(x, y);
        sum += Math.Abs(p.R - q.R) + Math.Abs(p.G - q.G) + Math.Abs(p.B - q.B); }
      if (sum < best) { best = sum; bx = ox; by = oy; } }
    return Scale(b, new Rectangle(bx, by, 320, 240)); }
  public static string Diff(Bitmap a, Bitmap b, Bitmap d) {
    long sum = 0; int bad = 0;
    for (int y = 0; y < 240; y++) for (int x = 0; x < 320; x++) {
      Color p = a.GetPixel(x, y), q = b.GetPixel(x, y);
      int dr = Math.Abs(p.R - q.R), dg = Math.Abs(p.G - q.G), db = Math.Abs(p.B - q.B);
      sum += dr + dg + db;
      if (Math.Max(dr, Math.Max(dg, db)) > 24) bad++;
      d.SetPixel(x, y, Color.FromArgb(Math.Min(255, dr * 4), Math.Min(255, dg * 4), Math.Min(255, db * 4))); }
    return String.Format(System.Globalization.CultureInfo.InvariantCulture,
                         "mean {0:F1}, differing {1:F1} %", sum / (3.0 * 320 * 240), bad * 100.0 / (320 * 240)); }
}
"@
$rows = $pairs.Split(';')
$m = New-Object System.Drawing.Bitmap 960, (240 * $rows.Count)
$g = [System.Drawing.Graphics]::FromImage($m)
$font = New-Object System.Drawing.Font 'Arial', 10
$i = 0
foreach ($row in $rows) {
  $name, $emuFile, $portFile = $row.Split('|')
  $port = [Compare]::Port([System.Drawing.Bitmap]::FromFile($portFile))
  $emu = [Compare]::Emulator([System.Drawing.Bitmap]::FromFile($emuFile), $port)
  $d = New-Object System.Drawing.Bitmap 320, 240
  $text = [Compare]::Diff($emu, $port, $d)
  "{0,-10} {1}" -f $name, $text
  $g.DrawImage($emu, 0, $i * 240)
  $g.DrawImage($port, 320, $i * 240)
  $g.DrawImage($d, 640, $i * 240)
  $g.DrawString("$name  $text", $font, [System.Drawing.Brushes]::Yellow, 644, $i * 240 + 4)
  $i++
}
$m.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
