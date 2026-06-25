param([string]$Path)
Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap($Path)
$w = $bmp.Width; $h = $bmp.Height
$colors = @{}
$step = 4
$total = 0
for ($y = 0; $y -lt $h; $y += $step) {
    for ($x = 0; $x -lt $w; $x += $step) {
        $c = $bmp.GetPixel($x, $y)
        $key = "{0},{1},{2}" -f $c.R, $c.G, $c.B
        if (-not $colors.ContainsKey($key)) { $colors[$key] = 0 }
        $colors[$key]++
        $total++
    }
}
Write-Host "Image: ${w}x${h}, sampled $total px (step $step), distinct colors: $($colors.Count)"
Write-Host "Top 6 colors:"
$colors.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 6 | ForEach-Object {
    $pct = [math]::Round(100.0 * $_.Value / $total, 1)
    Write-Host ("  ({0,-12}) {1,5}%  count={2}" -f $_.Key, $pct, $_.Value)
}
# center pixel
$cx = [int]($w/2); $cy = [int]($h/2)
$cc = $bmp.GetPixel($cx, $cy)
Write-Host "Center pixel ($cx,$cy): R=$($cc.R) G=$($cc.G) B=$($cc.B)"
$bmp.Dispose()
