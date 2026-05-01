Add-Type -AssemblyName System.Drawing
$b1 = New-Object System.Drawing.Bitmap 192, 192
$g1 = [System.Drawing.Graphics]::FromImage($b1)
$brush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 59, 130, 246))
$g1.FillRectangle($brush, 0, 0, 192, 192)
$font = New-Object System.Drawing.Font("Arial", 40)
$brushText = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::White)
$g1.DrawString("Exo", $font, $brushText, 40, 60)
$g1.Dispose()
$b1.Save("icon.png", [System.Drawing.Imaging.ImageFormat]::Png)
$b1.Dispose()
