# Measures fpng compression across a corpus. Prints per-image and total sizes.
# Usage: scripts\measure.ps1 [-Level 9] [-Out results.csv] [-Filter pattern]
param(
    [int]$Level = 9,
    [string]$Out = "",
    [string]$Filter = "*.png",
    [int]$Threads = 4,
    [string]$Exe = "build\bin\Release\fpng.exe",
    [string]$Corpus = "test_images"
)

$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$exePath = if ([System.IO.Path]::IsPathRooted($Exe)) { $Exe } else { Join-Path $root $Exe }
$corpusPath = if ([System.IO.Path]::IsPathRooted($Corpus)) { $Corpus } else { Join-Path $root $Corpus }
$tmp = Join-Path $env:TEMP ("fpng_measure_" + [System.IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Path $tmp | Out-Null

$images = Get-ChildItem -Path $corpusPath -Filter $Filter -File | Where-Object { $_.Name -like "*.png" } | Sort-Object Name
$results = @()
$totalOrig = 0
$totalOut = 0
$totalKept = 0

foreach ($img in $images) {
    $outFile = Join-Path $tmp $img.Name
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $output = & $exePath -o $Level -j $Threads -v $img.FullName $outFile 2>&1 | Out-String
    $sw.Stop()

    $origSize = $img.Length
    $outSize = $origSize
    $kept = $false

    if ($output -match 'Wrote:.*\((\d+) bytes') {
        $outSize = [int]$matches[1]
    } elseif ($output -match 'Compressed output not smaller \((\d+)') {
        $outSize = [int]$matches[1]
        $kept = $true
    }

    $totalOrig += $origSize
    if ($kept) { $totalKept += $origSize; $totalOut += $outSize }
    else { $totalOut += $outSize }

    $results += [PSCustomObject]@{
        Name = $img.Name
        Orig = $origSize
        Out  = $outSize
        Saved = $origSize - $outSize
        Kept = $kept
        Ms = [int]$sw.ElapsedMilliseconds
    }
}

Write-Host ""
Write-Host ("{0,-28} {1,10} {2,10} {3,10} {4,8}" -f "Image","Orig","Out","Saved","Time")
foreach ($r in $results) {
    $flag = if ($r.Kept) { "*" } else { " " }
    Write-Host ("{0,-28} {1,10} {2,10} {3,10} {4,6}ms{5}" -f $r.Name, $r.Orig, $r.Out, $r.Saved, $r.Ms, $flag)
}
Write-Host ""
Write-Host ("TOTAL: {0} images, orig {1} B, out {2} B, saved {3} B ({4:N2}%)" -f `
    $results.Count, $totalOrig, $totalOut, ($totalOrig - $totalOut), `
    (100.0 * ($totalOrig - $totalOut) / [Math]::Max(1,$totalOrig)))
Write-Host ("ACTUAL (kept-original counted): {0} B" -f ($totalOrig - ($results | Where-Object { -not $_.Kept } | Measure-Object -Property Saved -Sum).Sum))

if ($Out) {
    $results | Export-Csv -Path $Out -NoTypeInformation
    Write-Host "Wrote $Out"
}
