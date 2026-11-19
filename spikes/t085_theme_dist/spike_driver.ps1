# T-085 spike driver: builds a real .dockforge-theme payload (Compress-Archive),
# serves it from a loopback HttpListener origin, runs spike.exe, and writes
# spike_run.log. Everything under $env:TEMP\t085-spike; the zip lives in the
# spike directory so the log can reference it.
$ErrorActionPreference = 'Continue'
$Spike = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root  = Split-Path -Parent (Split-Path -Parent $Spike)   # repo root
$Log   = Join-Path $Spike 'spike_run.log'
if (Test-Path $Log) { Remove-Item $Log -Force }
$Work  = Join-Path $env:TEMP 't085-spike'
if (Test-Path $Work) { Remove-Item -Recurse -Force $Work }
New-Item -ItemType Directory -Force -Path $Work | Out-Null

# Payload: theme.json + preview.png + icons/ (C-02 shape).
$Payload = Join-Path $Work 'mono'
New-Item -ItemType Directory -Force -Path (Join-Path $Payload 'icons') | Out-Null
Set-Content -Path (Join-Path $Payload 'theme.json') -Value @'
{ "name": "mono", "author": "dockforge", "version": "1.0.0",
  "overrides": { "appearance": { "corner_radius": 12 } } }
'@
Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap 512, 288
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.Clear([System.Drawing.Color]::FromArgb(32, 32, 36))
$g.DrawString('mono', (New-Object System.Drawing.Font('Segoe UI', 24)), [System.Drawing.Brushes]::White, 20, 120)
$g.Dispose()
$bmp.Save((Join-Path $Payload 'preview.png'), [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Set-Content -Path (Join-Path $Payload 'icons\sample.png') -Value 'placeholder-not-a-real-png' -Encoding UTF8

# Compress-Archive only accepts .zip as the destination extension (measured:
# ".dockforge-theme 不是支持的存档文件格式"), so compress to .zip first; the .zip
# copy stays for the extraction probes (the shell's zip namespace keys on the
# .zip extension) and the .dockforge-theme copy is what gets served over HTTP.
$ZipTmp = Join-Path $Work 'mono.zip'
$Zip = Join-Path $Work 'mono.dockforge-theme'
Compress-Archive -Path (Join-Path $Payload '*') -DestinationPath $ZipTmp -CompressionLevel Optimal
Copy-Item $ZipTmp $Zip
$Sha = (Get-FileHash -Path $Zip -Algorithm SHA256).Hash.ToLowerInvariant()
"pack=$Zip size=$((Get-Item $Zip).Length) sha256=$Sha" | Out-File -FilePath $Log -Append -Encoding utf8

# Loopback origin: /pack.dockforge-theme serves the zip, everything else 404s.
# The job owns the listener exclusively - a second HttpListener on the same
# prefix in this process would accept connections and never answer them
# (measured: WinHTTP died with 12002 ERROR_WINHTTP_TIMEOUT). A leftover HTTP.sys
# registration from an earlier crashed run also blocks a fixed port, so the
# port is picked fresh per run and the job start is verified before use.
$Port = Get-Random -Minimum 18000 -Maximum 18999
$Prefix = "http://127.0.0.1:$Port/"
$ServerJob = Start-Job -ScriptBlock {
    param($Prefix, $ZipPath)
    $l = New-Object System.Net.HttpListener
    $l.Prefixes.Add($Prefix)
    $l.Start()
    while ($l.IsListening) {
        try {
            $ctx = $l.GetContext()
            if ($ctx.Request.Url.AbsolutePath -eq '/pack.dockforge-theme') {
                $bytes = [System.IO.File]::ReadAllBytes($ZipPath)
                $ctx.Response.ContentType = 'application/octet-stream'
                $ctx.Response.ContentLength64 = $bytes.Length
                $ctx.Response.OutputStream.Write($bytes, 0, $bytes.Length)
                $ctx.Response.StatusCode = 200
            } else {
                $ctx.Response.StatusCode = 404
            }
            $ctx.Response.OutputStream.Close()
        } catch { break }
    }
} -ArgumentList $Prefix, $Zip

# Poll until the job's listener actually answers (job cold-start measured slow).
$Ready = $false
for ($i = 0; $i -lt 60 -and -not $Ready; ++$i) {
    Start-Sleep -Milliseconds 250
    try {
        [System.Net.WebClient]::new().DownloadString("$Prefix/ready") | Out-Null
        $Ready = $true
    } catch {
        $resp = $_.Exception.InnerException.Response
        if ($resp -eq $null) { $resp = $_.Exception.Response }
        if ($resp -ne $null -and [int]$resp.StatusCode -eq 404) { $Ready = $true }
    }
}
"origin ready=$Ready port=$Port" | Out-File -FilePath $Log -Append -Encoding utf8
if (-not $Ready) {
    Write-Host ("job state=" + $ServerJob.State)
    Receive-Job $ServerJob -Keep | Write-Host
    throw "loopback origin never came up"
}
$Dest = Join-Path $Work 'extracted'
New-Item -ItemType Directory -Force -Path $Dest | Out-Null

$Exe = Join-Path $Spike 'spike.exe'
"run: $Exe $Port $Sha" | Out-File -FilePath $Log -Append -Encoding utf8
& $Exe $Port $Sha $Zip $Dest (Join-Path $Dest 'theme.json') 2>&1 |
    Out-File -FilePath $Log -Append -Encoding utf8
"spike exit=$LASTEXITCODE" | Out-File -FilePath $Log -Append -Encoding utf8

# Cleanup: kill the job (its listener dies with it), remove the scratch tree.
Stop-Job $ServerJob -ErrorAction SilentlyContinue
Remove-Job $ServerJob -Force -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force $Work -ErrorAction SilentlyContinue
"driver done" | Out-File -FilePath $Log -Append -Encoding utf8
