<#
.SYNOPSIS
    OCR every image in a folder (and its subfolders) using the OCR engine
    built into Windows 10/11 — no extra software required.

.DESCRIPTION
    Recursively finds image files under -Folder, runs each one through the
    Windows.Media.Ocr engine, and writes the recognized text to a .txt file
    next to each image (e.g. IMG_001.jpg -> IMG_001.txt).

.PARAMETER Folder
    Path to the folder to scan (scanned recursively, including subfolders).

.EXAMPLE
    .\Ocr-Images.ps1 -Folder "C:\Users\colewi00\Documents\Beehive"
#>

param(
    [Parameter(Mandatory = $true)]
    [string]$Folder
)

Add-Type -AssemblyName System.Runtime.WindowsRuntime

$AsTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.IsGenericMethod -and $_.GetGenericArguments().Count -eq 1
})[0]

function Await($WinRtTask, [type]$ResultType) {
    $AsTask = $AsTaskGeneric.MakeGenericMethod($ResultType)
    $Task = $AsTask.Invoke($null, @($WinRtTask))
    $Task.Wait(-1) | Out-Null
    $Task.Result
}

[Windows.Storage.StorageFile, Windows.Storage, ContentType = WindowsRuntime] | Out-Null
[Windows.Graphics.Imaging.BitmapDecoder, Windows.Graphics, ContentType = WindowsRuntime] | Out-Null
[Windows.Graphics.Imaging.SoftwareBitmap, Windows.Graphics, ContentType = WindowsRuntime] | Out-Null
[Windows.Media.Ocr.OcrEngine, Windows.Media, ContentType = WindowsRuntime] | Out-Null

$OcrEngine = [Windows.Media.Ocr.OcrEngine]::TryCreateFromUserProfileLanguages()
if (-not $OcrEngine) {
    Write-Error "No OCR language pack is installed. Go to Settings > Time & Language > Language & Region, add/check your language, and make sure 'Optical character recognition' is included, then try again."
    exit 1
}

$ImageExtensions = @(".jpg", ".jpeg", ".png", ".bmp", ".tif", ".tiff")

$ResolvedFolder = Resolve-Path -Path $Folder -ErrorAction SilentlyContinue
if (-not $ResolvedFolder -or -not (Test-Path -Path $ResolvedFolder.Path -PathType Container)) {
    Write-Error "Error: '$Folder' is not a valid folder."
    exit 1
}
$Folder = $ResolvedFolder.Path

$Images = @(Get-ChildItem -Path $Folder -File -Recurse | Where-Object { $ImageExtensions -contains $_.Extension.ToLower() })

if ($Images.Count -eq 0) {
    Write-Host "No image files found under '$Folder'."
    exit 0
}

Write-Host "Found $($Images.Count) images. Running OCR using language: $($OcrEngine.RecognizerLanguage.DisplayName)"
Write-Host "A .txt file will be written next to each image.`n"

$i = 0
$Failed = @()

foreach ($Img in $Images) {
    $i++
    Write-Host "[$i/$($Images.Count)] $($Img.FullName)"

    $Stream = $null
    try {
        $StorageFile = Await ([Windows.Storage.StorageFile]::GetFileFromPathAsync($Img.FullName)) ([Windows.Storage.StorageFile])
        $Stream = Await ($StorageFile.OpenAsync([Windows.Storage.FileAccessMode]::Read)) ([Windows.Storage.Streams.IRandomAccessStream])
        $Decoder = Await ([Windows.Graphics.Imaging.BitmapDecoder]::CreateAsync($Stream)) ([Windows.Graphics.Imaging.BitmapDecoder])
        $Bitmap = Await ($Decoder.GetSoftwareBitmapAsync()) ([Windows.Graphics.Imaging.SoftwareBitmap])

        # OcrEngine requires Bgra8 + Premultiplied (or Ignore) alpha; convert if the source isn't already in that format.
        if ($Bitmap.BitmapPixelFormat -ne [Windows.Graphics.Imaging.BitmapPixelFormat]::Bgra8 -or
            $Bitmap.BitmapAlphaMode -eq [Windows.Graphics.Imaging.BitmapAlphaMode]::Straight) {
            $Bitmap = [Windows.Graphics.Imaging.SoftwareBitmap]::Convert(
                $Bitmap,
                [Windows.Graphics.Imaging.BitmapPixelFormat]::Bgra8,
                [Windows.Graphics.Imaging.BitmapAlphaMode]::Premultiplied)
        }

        $OcrResult = Await ($OcrEngine.RecognizeAsync($Bitmap)) ([Windows.Media.Ocr.OcrResult])

        $TextOutPath = Join-Path $Img.DirectoryName ($Img.BaseName + ".txt")
        [System.IO.File]::WriteAllText($TextOutPath, $OcrResult.Text)
    }
    catch {
        Write-Warning "  ! Failed on $($Img.Name): $($_.Exception.Message)"
        $Failed += $Img.FullName
    }
    finally {
        if ($Stream) { $Stream.Dispose() }
    }
}

Write-Host "`nDone! Processed $($Images.Count) images."
if ($Failed.Count -gt 0) {
    Write-Host "$($Failed.Count) image(s) failed:"
    $Failed | ForEach-Object { Write-Host "  - $_" }
}
