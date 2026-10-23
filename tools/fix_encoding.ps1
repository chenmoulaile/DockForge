# tools/fix_encoding.ps1 - repair mixed UTF-8/ANSI (GBK) text files, and validate.
#
# Why this exists: PowerShell 5.1's Set-Content/Out-File default to the ANSI code
# page (936 on this machine), so a single "read -Raw, regex, Set-Content" edit
# silently rewrote the Chinese tail of a file as GBK while the head stayed UTF-8.
# The result is a file no UTF-8 reader can open, which is a broken state file -
# exactly the kind of thing the cold-start protocol depends on.
#
#   pwsh -File tools/fix_encoding.ps1 -Check        # report only, exit 1 if bad
#   pwsh -File tools/fix_encoding.ps1 -Repair       # convert GBK tails to UTF-8
#
# Repair strategy: find the first byte offset where strict UTF-8 decoding fails,
# walk back to the previous line break, decode the prefix as UTF-8 and the suffix
# as the ANSI code page, then write the joined text back as BOM-less UTF-8.
param(
    [switch]$Check,
    [switch]$Repair,
    [string[]]$Paths = @('*.md', '*.json')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$strict = New-Object System.Text.UTF8Encoding($false, $true)
$ansi = [System.Text.Encoding]::GetEncoding(
    [System.Globalization.CultureInfo]::CurrentCulture.TextInfo.ANSICodePage)
$utf8Out = New-Object System.Text.UTF8Encoding($false)

function Get-FirstInvalidByte {
    param([byte[]]$Bytes)
    # Decode incrementally so the reported offset is the real one. .NET's exception
    # message is localized ("无法将位于索引 8312 处的字节..."), and a regex over it
    # happily matches an unrelated digit, so the message is not a usable source.
    $n = $Bytes.Length
    $i = 0
    while ($i -lt $n) {
        $b = $Bytes[$i]
        $need = 0
        if ($b -lt 0x80) { $need = 0 }
        elseif (($b -band 0xE0) -eq 0xC0) { $need = 1 }
        elseif (($b -band 0xF0) -eq 0xE0) { $need = 2 }
        elseif (($b -band 0xF8) -eq 0xF0) { $need = 3 }
        else { return $i }
        if ($i + $need -ge $n) { return $i }
        for ($k = 1; $k -le $need; $k++) {
            if (($Bytes[$i + $k] -band 0xC0) -ne 0x80) { return $i }
        }
        $i += $need + 1
    }
    return -1
}

$bad = 0
foreach ($pattern in $Paths) {
    foreach ($file in Get-ChildItem -File -Path $pattern -ErrorAction SilentlyContinue) {
        $bytes = [System.IO.File]::ReadAllBytes($file.FullName)
        $at = Get-FirstInvalidByte $bytes
        if ($at -lt 0) {
            Write-Output ("OK   {0,-22} {1} bytes" -f $file.Name, $bytes.Length)
            continue
        }
        $bad++
        Write-Output ("BAD  {0,-22} {1} bytes, first invalid at {2}" -f $file.Name, $bytes.Length, $at)
        if (-not $Repair) { continue }

        # The corruption starts on a line boundary; everything before it is still
        # valid UTF-8 that a strict decode already accepted.
        $boundary = $at
        while ($boundary -gt 0 -and $bytes[$boundary - 1] -ne 0x0A) { $boundary-- }
        $head = $strict.GetString($bytes, 0, $boundary)
        $tail = $ansi.GetString($bytes, $boundary, $bytes.Length - $boundary)
        $repaired = $head + $tail
        [System.IO.File]::WriteAllText($file.FullName, $repaired, $utf8Out)

        $verify = [System.IO.File]::ReadAllBytes($file.FullName)
        $stillBad = Get-FirstInvalidByte $verify
        if ($stillBad -ge 0) {
            Write-Output ("     repair failed, still invalid at {0}" -f $stillBad)
        } else {
            Write-Output ("     repaired: {0} -> {1} bytes" -f $bytes.Length, $verify.Length)
        }
    }
}

if ($Check -and $bad -gt 0) { exit 1 }
