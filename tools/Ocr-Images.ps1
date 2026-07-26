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
# interleaves columns on multi-column pages (magazines, newspapers) -
# and worse, these photos are of open two-page spreads, so a photo can
# contain 2, 3, 4+ real text columns (each page's own columns, side by
# side). Rather than assume a fixed column count, this finds vertical
# "gutters" - strips of the content area that no word ever overlaps,
# anywhere in the image - and treats each one as a column boundary.
# A table's internal cell gaps don't qualify: they're only empty over
# the table's own row range, not the whole page height, so some other
# line's text elsewhere in that X range keeps the strip "occupied".
function Get-ReadingOrderText($OcrResult) {
    $WordRects = @()
    $LineInfos = @()
    foreach ($Line in $OcrResult.Lines) {
        if ($Line.Words.Count -eq 0) { continue }
        $MinX = ($Line.Words | ForEach-Object { $_.BoundingRect.X } | Measure-Object -Minimum).Minimum
        $MaxX = ($Line.Words | ForEach-Object { $_.BoundingRect.X + $_.BoundingRect.Width } | Measure-Object -Maximum).Maximum
        $MinY = ($Line.Words | ForEach-Object { $_.BoundingRect.Y } | Measure-Object -Minimum).Minimum
        $LineInfos += [PSCustomObject]@{ Text = $Line.Text; MinX = $MinX; MaxX = $MaxX; Y = $MinY }
        foreach ($Word in $Line.Words) {
            $WordRects += [PSCustomObject]@{ X1 = $Word.BoundingRect.X; X2 = $Word.BoundingRect.X + $Word.BoundingRect.Width }
        }
    }

    if ($LineInfos.Count -lt 4 -or $WordRects.Count -eq 0) {
        return ($LineInfos | Sort-Object Y | ForEach-Object { $_.Text }) -join "`n"
    }

    $ContentMinX = ($WordRects.X1 | Measure-Object -Minimum).Minimum
    $ContentMaxX = ($WordRects.X2 | Measure-Object -Maximum).Maximum
    $ContentWidth = $ContentMaxX - $ContentMinX

    if ($ContentWidth -le 0) {
        return ($LineInfos | Sort-Object Y | ForEach-Object { $_.Text }) -join "`n"
    }

    $NumBins = 150
    $BinWidth = $ContentWidth / $NumBins
    $Occupied = New-Object bool[] $NumBins

    foreach ($W in $WordRects) {
        $StartBin = [Math]::Max(0, [int][Math]::Floor(($W.X1 - $ContentMinX) / $BinWidth))
        $EndBin = [Math]::Min($NumBins - 1, [int][Math]::Ceiling(($W.X2 - $ContentMinX) / $BinWidth) - 1)
        for ($b = $StartBin; $b -le $EndBin; $b++) {
            $Occupied[$b] = $true
        }
    }

    # A gutter must be a run of never-occupied bins at least ~1% of the content width wide.
    $MinGutterBins = [Math]::Max(2, [int][Math]::Ceiling($NumBins * 0.01))
    $Boundaries = @()
    $RunStart = -1
    for ($b = 0; $b -lt $NumBins; $b++) {
        if (-not $Occupied[$b]) {
            if ($RunStart -eq -1) { $RunStart = $b }
        }
        elseif ($RunStart -ne -1) {
            $RunLength = $b - $RunStart
            if ($RunLength -ge $MinGutterBins) {
                $MidBin = $RunStart + ($RunLength / 2.0)
                $Boundaries += $ContentMinX + ($MidBin * $BinWidth)
            }
            $RunStart = -1
        }
    }

    if ($Boundaries.Count -eq 0) {
        return ($LineInfos | Sort-Object Y | ForEach-Object { $_.Text }) -join "`n"
    }

    $Boundaries = @($Boundaries | Sort-Object)
    $Bands = New-Object System.Collections.Generic.List[object]
    for ($i = 0; $i -le $Boundaries.Count; $i++) {
        $Bands.Add((New-Object System.Collections.Generic.List[object]))
    }

    foreach ($L in $LineInfos) {
        $Center = ($L.MinX + $L.MaxX) / 2.0
        $BandIndex = 0
        for ($i = 0; $i -lt $Boundaries.Count; $i++) {
            if ($Center -ge $Boundaries[$i]) { $BandIndex = $i + 1 }
        }
        $Bands[$BandIndex].Add($L)
    }

    $OrderedText = @()
    foreach ($Band in $Bands) {
        foreach ($L in ($Band | Sort-Object Y)) { $OrderedText += $L.Text }
    }

    return ($OrderedText -join "`n")
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
        $OrderedText = Get-ReadingOrderText $OcrResult

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
