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

# Windows OCR returns lines in raw top-to-bottom raster order, which
# interleaves left/right columns on multi-column pages (e.g. magazines,
# newspapers). This reconstructs proper reading order: detect a 2-column
# split from each line's horizontal position, then emit the left column
# top-to-bottom followed by the right column top-to-bottom. Falls back to
# simple top-to-bottom order when no clear column split is found.
function Get-ReadingOrderText($OcrResult, [double]$ImageWidth) {
    $LineInfos = @()
    foreach ($Line in $OcrResult.Lines) {
        if ($Line.Words.Count -eq 0) { continue }
        $MinX = ($Line.Words | ForEach-Object { $_.BoundingRect.X } | Measure-Object -Minimum).Minimum
        $MinY = ($Line.Words | ForEach-Object { $_.BoundingRect.Y } | Measure-Object -Minimum).Minimum
        $LineInfos += [PSCustomObject]@{ Text = $Line.Text; X = $MinX; Y = $MinY }
    }

    if ($LineInfos.Count -lt 6) {
        return ($LineInfos | Sort-Object Y | ForEach-Object { $_.Text }) -join "`n"
    }

    # Simple 1D k-means (k=2) on each line's left-edge X to find a column split.
    $Xs = @($LineInfos.X)
    $C1 = ($Xs | Measure-Object -Minimum).Minimum
    $C2 = ($Xs | Measure-Object -Maximum).Maximum
    for ($iter = 0; $iter -lt 10; $iter++) {
        $Group1 = @($Xs | Where-Object { [Math]::Abs($_ - $C1) -le [Math]::Abs($_ - $C2) })
        $Group2 = @($Xs | Where-Object { [Math]::Abs($_ - $C1) -gt [Math]::Abs($_ - $C2) })
        if ($Group1.Count -gt 0) { $C1 = ($Group1 | Measure-Object -Average).Average }
        if ($Group2.Count -gt 0) { $C2 = ($Group2 | Measure-Object -Average).Average }
    }

    $ColumnGap = [Math]::Abs($C2 - $C1)
    $MinGroupSize = 3

    if ($ColumnGap -gt ($ImageWidth * 0.15) -and $Group1.Count -ge $MinGroupSize -and $Group2.Count -ge $MinGroupSize) {
        $Threshold = ($C1 + $C2) / 2
        $LeftCol = @($LineInfos | Where-Object { $_.X -lt $Threshold } | Sort-Object Y)
        $RightCol = @($LineInfos | Where-Object { $_.X -ge $Threshold } | Sort-Object Y)
        $Ordered = @($LeftCol) + @($RightCol)
        return ($Ordered | ForEach-Object { $_.Text }) -join "`n"
    }

    return ($LineInfos | Sort-Object Y | ForEach-Object { $_.Text }) -join "`n"
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
        $OrderedText = Get-ReadingOrderText $OcrResult $Bitmap.PixelWidth

        $TextOutPath = Join-Path $Img.DirectoryName ($Img.BaseName + ".txt")
        [System.IO.File]::WriteAllText($TextOutPath, $OrderedText)
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
