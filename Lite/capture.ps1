param(
    [string]$Scene,
    [string]$Out = "capture.png",
    [int]$WaitMs = 4000,
    [hashtable]$Env = @{}
)

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Drawing;
using System.Drawing.Imaging;
public class Cap {
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    public static void Grab(IntPtr h, string path) {
        RECT r; GetWindowRect(h, out r);
        int w = r.R - r.L, ht = r.B - r.T;
        if (w <= 0 || ht <= 0) { w = 1280; ht = 720; }
        using (var bmp = new Bitmap(w, ht)) {
            using (var g = Graphics.FromImage(bmp)) {
                IntPtr hdc = g.GetHdc();
                PrintWindow(h, hdc, 2); // PW_RENDERFULLCONTENT
                g.ReleaseHdc(hdc);
            }
            bmp.Save(path, ImageFormat.Png);
        }
    }
}
"@ -ReferencedAssemblies System.Drawing

$env:LITE_SCENE_JS = $Scene
foreach ($k in $Env.Keys) { Set-Item -Path "env:$k" -Value $Env[$k] }

$exe = "D:\Repos\BabylonNative2\Lite\build\App\Release\LiteApp.exe"
Get-Process LiteApp -ErrorAction SilentlyContinue | ForEach-Object { Stop-Process -Id $_.Id -Force }
Start-Sleep -Milliseconds 300

$p = Start-Process -FilePath $exe -PassThru
Start-Sleep -Milliseconds $WaitMs

$proc = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
if ($proc -and $proc.MainWindowHandle -ne 0) {
    [Cap]::Grab($proc.MainWindowHandle, $Out)
    Write-Host "captured -> $Out (hwnd=$($proc.MainWindowHandle))"
} else {
    Write-Host "no window handle (process exited? id=$($p.Id))"
}

Get-Process LiteApp -ErrorAction SilentlyContinue | ForEach-Object { Stop-Process -Id $_.Id -Force }
