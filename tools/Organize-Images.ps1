<#
.SYNOPSIS
    Organize images into subfolders of 100.

.DESCRIPTION
    Looks at all image files directly inside the target folder, sorts them
    (by filename by default), and moves them in batches into subfolders
    named "1", "2", "3", and so on.

.PARAMETER Folder
    Path to the folder containing the images.

.PARAMETER DryRun
    Show what would happen without moving anything.

.PARAMETER ByDate
    Sort by last-write (modified) date instead of filename.

.PARAMETER BatchSize
    Number of images per subfolder (default 100).

.EXAMPLE
    .\Organize-Images.ps1 -Folder "C:\Users\me\Documents\beehive" -DryRun

.EXAMPLE
    .\Organize-Images.ps1 -Folder "C:\Users\me\Documents\beehive" -ByDate
#>

param(
    [Parameter(Mandatory = $true)]
    [string]$Folder,

    [switch]$DryRun,

    [switch]$ByDate,

    [int]$BatchSize = 100
)

$ImageExtensions = @(
    ".jpg", ".jpeg", ".png", ".gif", ".bmp", ".tif", ".tiff",
    ".heic", ".heif", ".webp", ".raw", ".cr2", ".nef", ".arw"
)

$ResolvedFolder = Resolve-Path -Path $Folder -ErrorAction SilentlyContinue
if (-not $ResolvedFolder -or -not (Test-Path -Path $ResolvedFolder.Path -PathType Container)) {
    Write-Error "Error: '$Folder' is not a valid folder."
    exit 1
}
$Folder = $ResolvedFolder.Path

$Images = @(Get-ChildItem -Path $Folder -File | Where-Object { $ImageExtensions -contains $_.Extension.ToLower() })

if ($Images.Count -eq 0) {
    Write-Host "No image files found directly inside '$Folder'."
    exit 0
}

if ($ByDate) {
    $Images = @($Images | Sort-Object LastWriteTime)
} else {
    $Images = @($Images | Sort-Object { $_.Name.ToLower() })
}

Write-Host "Found $($Images.Count) images in '$Folder'."
Write-Host "Will create subfolders of $BatchSize images each."
if ($DryRun) {
    Write-Host "--- DRY RUN: no files will actually be moved ---`n"
}

$BatchNum = 1
for ($i = 0; $i -lt $Images.Count; $i += $BatchSize) {
    $EndIndex = [Math]::Min($i + $BatchSize, $Images.Count) - 1
    $Batch = @($Images[$i..$EndIndex])
    $Subfolder = Join-Path $Folder $BatchNum

    Write-Host "Folder '$BatchNum': $($Batch.Count) images ($($Batch[0].Name) ... $($Batch[-1].Name))"

    if (-not $DryRun) {
        if (-not (Test-Path -Path $Subfolder)) {
            New-Item -Path $Subfolder -ItemType Directory | Out-Null
        }
        foreach ($Img in $Batch) {
            $Dest = Join-Path $Subfolder $Img.Name
            if (Test-Path -Path $Dest) {
                Write-Host "  ! Skipping $($Img.Name), already exists in $Subfolder"
                continue
            }
            Move-Item -Path $Img.FullName -Destination $Dest
        }
    }

    $BatchNum++
}

if ($DryRun) {
    Write-Host "`nDry run complete. Re-run without -DryRun to actually move the files."
} else {
    Write-Host "`nDone! Images have been moved into numbered subfolders."
}
