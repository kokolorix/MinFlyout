<#
.SYNOPSIS
    Konvertiert BrServerApp Debug-Logs in eine interaktive HTML-Ansicht.
    Kann mehrere Log-Dateien zusammenfuehren (z.B. Server + Client).

.PARAMETER LogFile
    Pfad zur Log-Datei(en). Mehrere Dateien werden zusammengefuehrt und nach Zeit sortiert.

.PARAMETER HtmlFile
    Optionaler Ausgabepfad. Standard: Name der ersten Datei mit .html

.PARAMETER Encoding
    Encoding. Standard: "auto" (erkennt UTF-8/BOM/Windows-1252). Oder: "1252", "utf-8", etc.

.PARAMETER Update
    Aktualisiert eine bestehende HTML-Datei mit neuen Log-Eintraegen.

.PARAMETER Serve
    Startet einen lokalen HTTP-Server mit Live-Refresh. Beenden mit Ctrl+C.

.PARAMETER Port
    Port fuer den HTTP-Server. Standard: 8099

.EXAMPLE
    .\BrServerLog2Html.ps1 .\app.log
    .\BrServerLog2Html.ps1 .\server.log .\client.log
    .\BrServerLog2Html.ps1 .\server.log .\client.log -Serve
    .\BrServerLog2Html.ps1 .\server.log .\client.log -HtmlFile merged.html
#>
param(
    [Parameter(Mandatory=$true, Position=0, ValueFromRemainingArguments=$true)]
    [string[]]$LogFile,
    [Parameter()]
    [Alias("o")]
    [string]$HtmlFile,
    [Parameter()]
    [string]$Encoding = "auto",
    [switch]$Update,
    [switch]$Serve,
    [int]$Port = 8099
)

$ErrorActionPreference = "Stop"
$sw = [System.Diagnostics.Stopwatch]::StartNew()

# Sicherheitscheck: Falls HtmlFile wie eine Log-Datei aussieht, zu LogFile verschieben
if ($HtmlFile -and $HtmlFile -match '\.(log|txt|csv)$') {
    $LogFile = @($LogFile) + @($HtmlFile)
    $HtmlFile = $null
}

$multiFile = $LogFile.Count -gt 1

function Detect-FileEncoding([string]$path) {
    $fs = [System.IO.FileStream]::new($path, [System.IO.FileMode]::Open,
          [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    $len = [Math]::Min($fs.Length, 100000)
    $bytes = New-Object byte[] $len
    [void]$fs.Read($bytes, 0, $len); $fs.Close()
    if ($len -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
        Write-Host "  Encoding: UTF-8 (BOM)"; return [System.Text.UTF8Encoding]::new($false)
    }
    if ($len -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) {
        Write-Host "  Encoding: UTF-16 LE"; return [System.Text.UnicodeEncoding]::new($false, $false)
    }
    $ok = $true; $i = 0; $hi = $false
    while ($i -lt $len) {
        $b = $bytes[$i]
        if ($b -gt 127) { $hi = $true
            if (($b -band 0xE0) -eq 0xC0) {
                if ($i+1 -ge $len -or ($bytes[$i+1] -band 0xC0) -ne 0x80) { $ok=$false; break }; $i+=2; continue
            } elseif (($b -band 0xF0) -eq 0xE0) {
                if ($i+2 -ge $len -or ($bytes[$i+1] -band 0xC0) -ne 0x80 -or ($bytes[$i+2] -band 0xC0) -ne 0x80) { $ok=$false; break }; $i+=3; continue
            } else { $ok=$false; break }
        }; $i++
    }
    if ($ok -and $hi) { Write-Host "  Encoding: UTF-8"; return [System.Text.UTF8Encoding]::new($false) }
    Write-Host "  Encoding: Windows-1252"; return [System.Text.Encoding]::GetEncoding(1252)
}

function JsonEnc([string]$s) {
    if (-not $s) { return "" }
    $s = $s -replace '\\', '\\'; $s = $s -replace '"', '\"'
    $s = $s -replace "`r`n", '\n'; $s = $s -replace "`r", '\n'
    $s = $s -replace "`n", '\n'; $s = $s -replace "`t", '\t'
    return $s
}

function ReadLogFile([string]$path, $enc) {
    $fs = [System.IO.FileStream]::new($path, [System.IO.FileMode]::Open,
          [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    $sr = [System.IO.StreamReader]::new($fs, $enc)
    $c = $sr.ReadToEnd(); $sr.Close(); $fs.Close(); return $c
}

function LimitLines([string]$content, [int]$maxLines = 50000) {
    $nl = $content.IndexOf("`n")
    if ($nl -lt 0) { return $content }
    $header = $content.Substring(0, $nl + 1)
    $body = $content.Substring($nl + 1)
    $lines = $body -split "`n"
    if ($lines.Count -le $maxLines) { return $content }
    $skip = $lines.Count - $maxLines
    Write-Host "  Warnung: $($lines.Count + 1) Zeilen - nur letzte $maxLines verarbeitet ($skip uebersprungen)" -ForegroundColor Yellow
    return $header + ($lines[$skip..($lines.Count - 1)] -join "`n")
}

function ResolveEncoding([string]$path) {
    if ($Encoding -eq "auto") { return Detect-FileEncoding $path }
    elseif ($Encoding -match '^\d+$') { return [System.Text.Encoding]::GetEncoding([int]$Encoding) }
    else { return [System.Text.Encoding]::GetEncoding($Encoding) }
}

function MakeFileTag([string]$path) {
    $n = [System.IO.Path]::GetFileNameWithoutExtension($path)
    $n = $n -replace '[._-]exe.*$', '' -replace '[._-]Debug\d*$', '' -replace '[._-]Release\d*$', ''
    return $n
}

function AppendEntryJson([System.Text.StringBuilder]$sb, $e, [int]$lineNr, [string]$tag, [bool]$comma) {
    if ($comma) { [void]$sb.Append(",") }
    [void]$sb.Append("[")
    [void]$sb.Append("$lineNr,")
    [void]$sb.Append("`"$(JsonEnc $e.Zeit)`",")
    [void]$sb.Append("`"$(JsonEnc $e.LogLevel)`",")
    [void]$sb.Append("`"$(JsonEnc $e.Thread)`",")
    [void]$sb.Append("`"$(JsonEnc $e.'Duration(ms)')`",")
    [void]$sb.Append("`"$(JsonEnc $e.Function)`",")
    [void]$sb.Append("`"$(JsonEnc $e.Message)`",")
    [void]$sb.Append("`"$(JsonEnc $e.LogSource)`",")
    [void]$sb.Append("`"$(JsonEnc $tag)`",")
    [void]$sb.Append("`"$(JsonEnc $e.Detail)`",")
    [void]$sb.Append("`"$(JsonEnc $e.Callstack)`",")
    [void]$sb.Append("`"$(JsonEnc $e.Prozess)`"")
    [void]$sb.Append("]")
}

# Ausgabedatei
if ([string]::IsNullOrEmpty($HtmlFile)) {
    if ($multiFile) { $HtmlFile = [System.IO.Path]::ChangeExtension($LogFile[0], ".merged.html") }
    else { $HtmlFile = [System.IO.Path]::ChangeExtension($LogFile[0], ".html") }
}
$sourceName = if ($multiFile) {
    ($LogFile | ForEach-Object { [System.IO.Path]::GetFileName($_) }) -join " + "
} else { [System.IO.Path]::GetFileName($LogFile[0]) }

# ═══ UPDATE-Modus ═══
if ($Update) {
    if (-not (Test-Path $HtmlFile)) { Write-Host "HTML nicht vorhanden - erstelle neu..."; $Update = $false }
    else {
        Write-Host "Update-Modus..."
        $htmlContent = [System.IO.File]::ReadAllText($HtmlFile, [System.Text.UTF8Encoding]::new($false))
        $metaM = [regex]::Match($htmlContent, 'LOGMETA:entries=(\d+)')
        if (-not $metaM.Success) { Write-Host "  Keine Metadaten - neu..."; $Update = $false }
        else {
            $existingCount = [int]$metaM.Groups[1].Value
            $jStart = $htmlContent.IndexOf('var D=[')
            $nMarker = $htmlContent.IndexOf('var N=D.length')
            $jSrcMarker = $htmlContent.IndexOf('var SRC=', $jStart)
            $jEnd = if ($jSrcMarker -gt 0) { $htmlContent.LastIndexOf('];', $jSrcMarker) } else { $htmlContent.LastIndexOf('];', $nMarker) }
            if ($jStart -lt 0 -or $jEnd -lt 0) { Write-Host "  Kein JSON - neu..."; $Update = $false }
            else {
                $existingJson = $htmlContent.Substring($jStart + 6, $jEnd - $jStart - 6 + 1)
                $allEntries = @(); $sourceList = @()
                foreach ($lf in $LogFile) {
                    $enc = ResolveEncoding $lf; $c = ReadLogFile $lf $enc
                    $c = LimitLines $c
                    $fe = $c | ConvertFrom-Csv -Delimiter "`t"; $tag = MakeFileTag $lf
                    if ($sourceList -notcontains $tag) { $sourceList += $tag }
                    foreach ($e in $fe) { Add-Member -InputObject $e -NotePropertyName '_Tag' -NotePropertyValue $tag -Force }
                    $allEntries += $fe
                }
                if ($multiFile) { $allEntries = $allEntries | Sort-Object { $_.Zeit } }
                $totalInLog = $allEntries.Count; $newCount = $totalInLog - $existingCount

                if ($newCount -le 0) { $sw.Stop(); Write-Host "Keine neuen Eintraege."; exit 0 }

                Write-Host "  $newCount neue Eintraege"
                $sb = [System.Text.StringBuilder]::new(5MB)
                $levelCounts = @{ "ERROR"=0; "WARNING"=0; "INFO"=0; "DEBUG"=0; "TRACE"=0 }
                # Badge-Counts lesen: CSS ist ~7500 Zeichen, Badges bei ~7800
                $hChunk = $htmlContent.Substring(0, [Math]::Min(12000, $htmlContent.Length))
                foreach ($lv in @("ERROR","WARNING","INFO","DEBUG","TRACE")) {
                    $sM = [regex]::Match($hChunk, "${lv}:\s*(\d+)")
                    if ($sM.Success) { $levelCounts[$lv] = [int]$sM.Groups[1].Value }
                }
                $threads = @{}; $lastTime = ""
                for ($i = $existingCount; $i -lt $totalInLog; $i++) {
                    $e = $allEntries[$i]; $tag = if ($e._Tag) { $e._Tag } else { "" }
                    $lvl = if ($e.LogLevel) { $e.LogLevel.ToUpper() } else { "" }
                    if ($levelCounts.ContainsKey($lvl)) { $levelCounts[$lvl]++ }
                    if ($e.Thread) { $threads[$e.Thread] = $true }
                    if ($e.Zeit) { $lastTime = $e.Zeit }
                    AppendEntryJson $sb $e ($i + 2) $tag $true
                }

                # Nur die schliessende Klammer des Arrays entfernen: TrimEnd(']') nahm
                # auch die der letzten Zeile mit. Bei leerem Array kein fuehrendes Komma.
                $body = $existingJson.Substring(0, $existingJson.Length - 1)
                $newRows = $sb.ToString()
                if ($body.TrimEnd().EndsWith('[')) { $newRows = $newRows.TrimStart(',') }
                $jsonData = $body + $newRows + "]"
                $total = $totalInLog; $threadCount = $threads.Count
                $titleMatch = [regex]::Match($htmlContent, '<title>(.+?)\s*-\s*Log Viewer</title>')
                if ($titleMatch.Success) { $sourceName = $titleMatch.Groups[1].Value }
                $ftMatch = [regex]::Match($htmlContent, '\|&nbsp;\s*([\d:.]+)\s*&#')
                $firstTime = if ($ftMatch.Success) { $ftMatch.Groups[1].Value } else { "" }
                $tcMatch = [regex]::Match($htmlContent, '(\d+)\s*Threads')
                if ($tcMatch.Success) { $threadCount = [Math]::Max($threadCount, [int]$tcMatch.Groups[1].Value) }
                $processName = ""
            }
        }
    }
}

# ═══ NEU-Modus ═══
if (-not $Update) {
    $allEntries = @(); $sourceList = @()
    foreach ($lf in $LogFile) {
        Write-Host "Lese: $lf"
        $enc = ResolveEncoding $lf; $c = ReadLogFile $lf $enc
        $c = LimitLines $c
        $fe = $c | ConvertFrom-Csv -Delimiter "`t"; $tag = MakeFileTag $lf
        $sourceList += $tag
        Write-Host "  $($fe.Count) Eintraege als '$tag'"
        foreach ($e in $fe) { Add-Member -InputObject $e -NotePropertyName '_Tag' -NotePropertyValue $tag -Force }
        $allEntries += $fe
    }
    if ($multiFile) {
        Write-Host "Sortiere $($allEntries.Count) Eintraege nach Zeit..."
        $allEntries = $allEntries | Sort-Object { $_.Zeit }
    }
    $total = $allEntries.Count
    Write-Host "Generiere JSON ($total Eintraege)..."
    $levelCounts = @{ "ERROR"=0; "WARNING"=0; "INFO"=0; "DEBUG"=0; "TRACE"=0 }
    $threads = @{}; $firstTime = ""; $lastTime = ""; $processName = ""; $processes = @{}
    $sb = [System.Text.StringBuilder]::new(50MB)
    [void]$sb.Append("[")
    for ($i = 0; $i -lt $total; $i++) {
        $e = $allEntries[$i]; $tag = if ($e._Tag) { $e._Tag } else { "" }
        $lvl = if ($e.LogLevel) { $e.LogLevel.ToUpper() } else { "" }
        if ($levelCounts.ContainsKey($lvl)) { $levelCounts[$lvl]++ }
        if ($e.Thread) { $threads[$e.Thread] = $true }
        # Mehrere Instanzen schreiben nicht chronologisch: Minimum und Maximum.
        if ($e.Zeit -and (-not $firstTime -or $e.Zeit -lt $firstTime)) { $firstTime = $e.Zeit }
        if ($e.Zeit -and $e.Zeit -gt $lastTime) { $lastTime = $e.Zeit }
        if ($e.Prozess) { $processes[$e.Prozess] = $true; if (-not $processName) { $processName = $e.Prozess } }
        AppendEntryJson $sb $e ($i + 2) $tag ($i -gt 0)
        if (($i + 1) % 2000 -eq 0) { Write-Host -NoNewline "`r  $($i+1) / $total..." }
    }
    [void]$sb.Append("]")
    Write-Host "`r  $total / $total konvertiert.     "
    $jsonData = $sb.ToString()
    $threadCount = $threads.Count
    # Mehrere Instanzen im selben Log: im Untertitel die Anzahl statt nur der ersten.
    if ($processes.Count -gt 1) { $processName = "$($processes.Count) Prozesse" }
}

# ═══ HTML generieren ═══
Write-Host "Schreibe HTML..."
$statsBadges = ""
if ($levelCounts["ERROR"] -gt 0)   { $statsBadges += "<span class='sb e'>ERROR: $($levelCounts['ERROR'])</span>" }
if ($levelCounts["WARNING"] -gt 0) { $statsBadges += "<span class='sb w'>WARNING: $($levelCounts['WARNING'])</span>" }
if ($levelCounts["INFO"] -gt 0)    { $statsBadges += "<span class='sb i'>INFO: $($levelCounts['INFO'])</span>" }
if ($levelCounts["DEBUG"] -gt 0)   { $statsBadges += "<span class='sb d'>DEBUG: $($levelCounts['DEBUG'])</span>" }
if ($levelCounts["TRACE"] -gt 0)   { $statsBadges += "<span class='sb t'>TRACE: $($levelCounts['TRACE'])</span>" }

$sourceButtons = ""; $sourceJsonArr = "[]"
if ($sourceList.Count -gt 1) {
    $sourceJsonArr = "[" + (($sourceList | ForEach-Object { "`"$_`"" }) -join ",") + "]"
    $ci = 0
    foreach ($s in $sourceList) {
        $sourceButtons += "<button class='fb src$ci on' onclick=`"tsf(this,'$s')`">$s</button>"
        $ci++
    }
    $sourceButtons += "<span class='sep'>|</span>"
}

$htmlDoc = @"
<!-- LOGMETA:entries=$total -->
<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="UTF-8">
<title>$sourceName - Log Viewer</title>
<style>
:root{--bg:#e8ecf0;--card:#fff;--hdr:#1e3a5f;--t1:#1a1a2e;--t2:#4a5568;--th:#fff;--brd:#c8ced6;--acc:#2563a8;--acc-l:#dbeafe;--rh:#e1edfb;--rs:#c6dcf5;--ce:#d32f2f;--ceb:#ffebee;--cw:#e67c00;--cwb:#fff3e0;--ci:#1565c0;--cib:#e3f2fd;--cd:#546e7a;--cdb:#eceff1;--ct:#78909c;--ctb:#f5f5f5;--mono:'Cascadia Code','Fira Code','Consolas',monospace;--ui:'Segoe UI',system-ui,sans-serif;--ROW:26;--cs0:#7c3aed;--cs0b:#ede9fe;--cs1:#0891b2;--cs1b:#e0f7fa;--cs2:#b45309;--cs2b:#fef3c7;--cs3:#059669;--cs3b:#d1fae5;--cs4:#dc2626;--cs4b:#fee2e2}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:var(--ui);font-size:13px;background:var(--bg);color:var(--t1);overflow:hidden;height:100vh;display:flex;flex-direction:column}
.hd{background:var(--hdr);color:var(--th);padding:10px 20px;display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:8px;flex-shrink:0}
.hd h1{font-size:16px;font-weight:600}.hd .sub{font-size:11px;opacity:.75;margin-top:1px}
.hs{display:flex;gap:6px;align-items:center;flex-wrap:wrap}
.sb{padding:2px 9px;border-radius:12px;font-size:10px;font-weight:700;display:inline-block}
.sb.e{background:var(--ce);color:#fff}.sb.w{background:var(--cw);color:#fff}.sb.i{background:var(--ci);color:#fff}.sb.d{background:var(--cd);color:#fff}.sb.t{background:var(--ct);color:#fff}
.stot{font-size:11px;opacity:.8;margin-right:6px}
.tb{padding:8px 20px;display:flex;gap:8px;align-items:center;flex-wrap:wrap;background:var(--card);border-bottom:1px solid var(--brd);flex-shrink:0}
.sep{color:var(--brd);font-size:16px;margin:0 2px}
#si{flex:1;min-width:180px;padding:6px 12px;font-size:13px;border:1px solid var(--brd);border-radius:4px;outline:none;font-family:var(--ui)}
#si:focus{border-color:var(--acc);box-shadow:0 0 0 2px rgba(37,99,168,.15)}
.fb{padding:4px 12px;font-size:11px;font-weight:700;border:1px solid var(--brd);border-radius:4px;background:var(--card);cursor:pointer;user-select:none}.fb:hover{background:#f0f4f8}
.fb.on.e{background:var(--ceb);border-color:var(--ce);color:var(--ce)}.fb.on.w{background:var(--cwb);border-color:var(--cw);color:var(--cw)}
.fb.on.i{background:var(--cib);border-color:var(--ci);color:var(--ci)}.fb.on.d{background:var(--cdb);border-color:var(--cd);color:var(--cd)}
.fb.on.t{background:var(--ctb);border-color:var(--ct);color:var(--ct)}
.fb.on.src0{background:var(--cs0b);border-color:var(--cs0);color:var(--cs0)}.fb.on.src1{background:var(--cs1b);border-color:var(--cs1);color:var(--cs1)}
.fb.on.src2{background:var(--cs2b);border-color:var(--cs2);color:var(--cs2)}.fb.on.src3{background:var(--cs3b);border-color:var(--cs3);color:var(--cs3)}
.fb.on.src4{background:var(--cs4b);border-color:var(--cs4);color:var(--cs4)}
.fb.rx{font-family:var(--mono);font-size:13px;font-weight:700;letter-spacing:-1px;color:var(--t2)}.fb.rx.on{background:var(--acc-l);border-color:var(--acc);color:var(--acc)}
#si.rxerr{border-color:var(--ce);box-shadow:0 0 0 2px rgba(211,47,47,.2);background:#fff5f5}
#rxhint{font-size:10px;color:var(--ce);display:none;margin-left:4px}
.fb.rf{color:#0a7d3e;border-color:#0a7d3e;font-size:12px;padding:4px 14px}.fb.rf:hover{background:#e6f4ec}.fb.rf.spin{pointer-events:none;opacity:.6}
.fb.st{color:#ab3535;border-color:#ab3535;font-size:11px;padding:4px 10px}.fb.st:hover{background:#fce8e8}
#vc{font-size:11px;color:var(--t2);margin-left:auto;white-space:nowrap}
.tw{flex:1;overflow-y:auto;overflow-x:hidden;position:relative;background:var(--card)}
.thead{position:sticky;top:0;z-index:10;display:flex;background:#edf0f4;border-bottom:2px solid var(--brd)}
.th{padding:6px 8px;font-size:10px;font-weight:700;text-transform:uppercase;letter-spacing:.5px;color:var(--t2);cursor:pointer;user-select:none;white-space:nowrap;overflow:hidden;position:relative;flex-shrink:0}
.th:hover{background:#dde3ea}.th::after{content:' \21C5';color:#aaa;font-size:9px}
.th.sa::after{content:' \2191';color:var(--acc)}.th.sd::after{content:' \2193';color:var(--acc)}
.resizer{position:absolute;top:0;right:0;width:5px;height:100%;cursor:col-resize;user-select:none}
.c0{width:55px}.c1{width:105px}.c2{width:72px;text-align:center}.c3{width:155px}.c4{width:68px;text-align:right}.c5{width:170px}.c6{flex:1;min-width:150px}.c7{width:200px}.c8{width:110px}.c9{width:560px}.c11{width:170px}
#vscroll{position:relative}
.vr{position:absolute;left:0;right:0;display:flex;align-items:center;cursor:pointer;border-bottom:1px solid #edf0f4;overflow:hidden}
.vr:hover{background:var(--rh)}.vr.sel{background:var(--rs)!important}
.vr.ev{background:#f7f9fb}.vr.od{background:#fff}
.vr.le{border-left:3px solid var(--ce)}.vr.lw{border-left:3px solid var(--cw)}
.vc{padding:0 8px;overflow:hidden;white-space:nowrap;text-overflow:ellipsis;flex-shrink:0;line-height:calc(var(--ROW)*1px)}
.vc.c1,.vc.c4{font-family:var(--mono);font-size:11px}.vc.c7,.vc.c8{font-size:10px;color:var(--t2)}
.stag{display:inline-block;padding:1px 6px;border-radius:3px;font-size:9px;font-weight:700}
.stag.src0{background:var(--cs0b);color:var(--cs0)}.stag.src1{background:var(--cs1b);color:var(--cs1)}.stag.src2{background:var(--cs2b);color:var(--cs2)}.stag.src3{background:var(--cs3b);color:var(--cs3)}.stag.src4{background:var(--cs4b);color:var(--cs4)}
mark{display:inline-block;padding:1px 6px;border-radius:3px;font-size:9px;font-weight:700;border:none;min-width:48px;text-align:center}
mark.ERROR{background:var(--ceb);color:var(--ce)}mark.WARNING{background:var(--cwb);color:var(--cw)}mark.INFO{background:var(--cib);color:var(--ci)}mark.DEBUG{background:var(--cdb);color:var(--cd)}mark.TRACE{background:var(--ctb);color:var(--ct)}
#panel{position:fixed;top:0;right:0;width:50%;height:100%;background:var(--card);z-index:200;box-shadow:-4px 0 20px rgba(0,0,0,.18);display:none;flex-direction:column;border-left:1px solid var(--brd)}
#panel.open{display:flex}
._ph{display:flex;background:var(--hdr);color:var(--th);min-height:38px;user-select:none}
._pt{flex:1;padding:8px 14px;font-size:12px;font-weight:600;cursor:move;display:flex;align-items:center;overflow:hidden;white-space:nowrap}
._pb{width:38px;display:flex;align-items:center;justify-content:center;cursor:pointer;font-size:15px;border:none;background:0 0;color:#fff}._pb:hover{background:rgba(255,255,255,.15)}._pb.cl:hover{background:#ab3535}
.pby{flex:1;overflow-y:auto}.ds{border-bottom:1px solid #e2e8f0}
.dsh{background:var(--acc-l);color:var(--acc);padding:7px 14px;font-weight:700;font-size:11px;text-transform:uppercase;letter-spacing:.4px;cursor:pointer;user-select:none;display:flex;align-items:center;gap:5px}
.dsh::before{content:'\25BC';font-size:8px;transition:transform .2s}.dsh.x::before{transform:rotate(-90deg)}
.dsc{padding:10px 14px;overflow:auto}.dsc.x{display:none}
.dsc pre{font-family:var(--mono);font-size:11px;line-height:1.5;white-space:pre-wrap;word-break:break-all;margin:0}
.dm{display:grid;grid-template-columns:100px 1fr;gap:3px 10px;font-size:11px}
.dm dt{font-weight:700;color:var(--t2);text-transform:uppercase;font-size:9px;letter-spacing:.4px;padding-top:2px}
.dm dd{font-family:var(--mono);font-size:11px;word-break:break-all}
.csl{display:flex;gap:6px;padding:1px 0;font-family:var(--mono);font-size:10px}.csl:hover{background:#f0f4f8}
.csn{color:var(--t2);min-width:22px;text-align:right}.csm{color:var(--acc);min-width:180px}.csf{color:var(--t1);word-break:break-all}
::-webkit-scrollbar{width:8px;height:8px}::-webkit-scrollbar-track{background:#f1f1f1}::-webkit-scrollbar-thumb{background:#b0b8c4;border-radius:4px}
@media(max-width:1200px){#panel{width:100%}}
.cf-btn{position:absolute;right:5px;top:50%;transform:translateY(-50%);font-size:9px;cursor:pointer;color:#aaa;padding:2px 3px;line-height:1;user-select:none;border-radius:2px;background:none;border:none}.cf-btn:hover{color:#333;background:rgba(0,0,0,.08)}.cf-btn.active{color:#0a7d3e;font-weight:bold}
.cf-panel{position:fixed;background:#fff;border:1px solid #ccc;border-radius:4px;box-shadow:0 4px 14px rgba(0,0,0,.18);z-index:9999;min-width:210px;max-width:340px;font-size:13px}
.cf-search{padding:7px 8px 4px}.cf-search input{width:100%;padding:4px 6px;border:1px solid #ccc;border-radius:3px;font-size:12px;box-sizing:border-box}
.cf-list{max-height:230px;overflow-y:auto;padding:3px 0;border-top:1px solid #eee;border-bottom:1px solid #eee}
.cf-item{display:flex;align-items:center;gap:6px;padding:3px 10px;cursor:pointer;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.cf-item:hover{background:#f0f4ff}
.cf-all{font-style:italic;border-bottom:1px solid #eee;margin-bottom:2px}.cf-footer{display:flex;justify-content:flex-end;gap:6px;padding:6px 8px}
.cf-ok{padding:4px 14px;background:#0a7d3e;color:#fff;border:none;border-radius:3px;cursor:pointer;font-size:12px}
.cf-cancel{padding:4px 10px;background:#f2f2f2;border:1px solid #ccc;border-radius:3px;cursor:pointer;font-size:12px}
</style>
</head>
<body>
<div class="hd">
 <div><h1>&#128269; $sourceName</h1>
  <div class="sub">$processName &nbsp;|&nbsp; $firstTime &#8211; $lastTime &nbsp;|&nbsp; $threadCount Threads</div></div>
 <div class="hs"><span class="stot">$total Eintr.</span>$statsBadges</div>
</div>
<div class="tb">
 <input type="text" id="si" placeholder="Protokoll durchsuchen... (Ctrl+F)"/>
 <button class="fb rx" id="rxBtn" onclick="tRx()" title="Regex Ein/Aus">.*</button>
 $sourceButtons
 <span id="vc">$total / $total</span><span id="rxhint"></span>
 <button class="fb rf" id="rfBtn" onclick="doRefresh()" title="Refresh" style="display:none">&#x21BB; Aktualisieren</button>
 <button class="fb st" id="stBtn" onclick="doStop()" title="Server beenden" style="display:none">&#x25A0; Stop</button>
</div>
<div class="tw" id="tw">
 <div class="thead" id="thd">
  <div class="th c0" data-c="0" data-t="n">Zeile<div class="resizer"></div></div>
  <div class="th c1" data-c="1" data-t="s">Zeit<div class="resizer"></div></div>
  <div class="th c2" data-c="2" data-t="s">Level<div class="resizer"></div></div>
  <div class="th c11" data-c="11" data-t="s">Prozess<div class="resizer"></div></div>
  <div class="th c3" data-c="3" data-t="s">Thread<div class="resizer"></div></div>
  <div class="th c4" data-c="4" data-t="n">Dauer(ms)<div class="resizer"></div></div>
  <div class="th c5" data-c="5" data-t="s">Funktion<div class="resizer"></div></div>
  <div class="th c6" data-c="6" data-t="s">Meldung<div class="resizer"></div></div>
  <div class="th c9" data-c="9" data-t="s">Details<div class="resizer"></div></div>
  <div class="th c7" data-c="7" data-t="s">Quelle<div class="resizer"></div></div>
  <div class="th c8" data-c="8" data-t="s">Datei<div class="resizer"></div></div>
 </div>
 <div id="vscroll"></div>
</div>
<div id="panel">
 <div class="_ph" id="phdr"><div class="_pt" id="ptit">Details</div>
  <button class="_pb" onclick="pDk()">&#8847;</button><button class="_pb" onclick="pTg()">&#8862;</button><button class="_pb cl" onclick="pCl()">&#10005;</button></div>
 <div class="pby" id="pby"></div>
</div>
<script>
// D[i]=[line,zeit,level,thread,duration,function,message,logsource,fileTag,detail,callstack,prozess]
//       0    1    2     3      4        5        6       7         8       9      10        11
// Index 12 wird beim Laden mit dem Suchtext der sichtbaren Spalten belegt.
var D=$jsonData;
var SRC=$sourceJsonArr;
var MF=SRC.length>1;
var N=D.length,RH=26;
var colOrder=[0,1,2,11,3,4,5,6,9,7,8];
var F=[],sC=-1,sD=1,sTm='',sF=new Set(SRC),useRx=false,rxObj=null;
var _cf={},_cfPanel=null,_cfCol=-1;
var sI=-1,sDI=-1;
var srcCls={};SRC.forEach(function(s,i){srcCls[s]='src'+i});
var tw=document.getElementById('tw'),vs=document.getElementById('vscroll');
var thCols=document.querySelectorAll('.th');
var searchTimer=null;
if(!MF){var c8=document.querySelector('.th.c8');if(c8)c8.style.display='none'}
(function(){
 for(var i=0;i<N;i++){var r=D[i],s='';if(r[11]==null)r[11]='';for(var k=0;k<colOrder.length;k++)s+=String(r[colOrder[k]]).toLowerCase()+'\t';r[12]=s}
 // Standard: TRACE herausfiltern
 (function(){var all=new Set();for(var i=0;i<N;i++){var lv=(D[i][2]||'').toUpperCase();if(lv)all.add(lv)}
  var def=new Set(all);def.delete('TRACE');if(def.size>0)_cf[2]=def})();
 setupTH();initColumnFilters();
 (function(){var s=sessionStorage.getItem('lgFS');if(!s)return;try{var st=JSON.parse(s);sessionStorage.removeItem('lgFS');if(st.t){sTm=st.t;document.getElementById('si').value=st.t}if(st.rx){useRx=true;document.getElementById('rxBtn').classList.add('on');document.getElementById('si').placeholder='Regex-Filter...';buildRx()}if(st.cf){Object.keys(st.cf).forEach(function(k){_cf[+k]=new Set(st.cf[k]);var btn=document.querySelector('.th[data-c="'+k+'"] .cf-btn');if(btn)btn.classList.add('active')})}if(st.sf&&MF){sF=new Set(st.sf);document.querySelectorAll('.fb').forEach(function(b){var m=b.getAttribute('onclick');if(m){var n=m.match(/tsf\(this,'(.+?)'\)/);if(n)b.classList.toggle('on',sF.has(n[1]))}})}}catch(e){}})();
 applyF();
 // Default: neueste Eintraege zuoberst (Zeit absteigend)
 sC=1;sD=-1;thCols.forEach(function(h){h.classList.remove('sa','sd')});var _tz=document.querySelector('.th[data-c="1"]');if(_tz)_tz.classList.add('sd');F.sort(function(a,b){return String(D[b][1]).localeCompare(String(D[a][1]))});lS=-1;
 tw.addEventListener('scroll',render);
 window.addEventListener('resize',function(){setVH();lS=-1;render()});
 document.getElementById('si').addEventListener('input',function(){
  if(searchTimer)clearTimeout(searchTimer);var v=this.value;
  searchTimer=setTimeout(function(){sTm=v;buildRx();applyF()},200)});
 document.addEventListener('keydown',onKey);render()})();
function tRx(){useRx=!useRx;document.getElementById('rxBtn').classList.toggle('on',useRx);
 document.getElementById('si').placeholder=useRx?'Regex-Filter...':'Protokoll durchsuchen... (Ctrl+F)';buildRx();applyF()}
function buildRx(){rxObj=null;var h=document.getElementById('rxhint'),inp=document.getElementById('si');
 h.style.display='none';inp.classList.remove('rxerr');if(!useRx||!sTm)return;
 try{rxObj=new RegExp(sTm,'i')}catch(e){inp.classList.add('rxerr');h.textContent=e.message;h.style.display='inline'}}
function setupTH(){thCols.forEach(function(th){
 th.addEventListener('click',function(e){if(!e.target.classList.contains('resizer'))doSort(th)});
 var rs=th.querySelector('.resizer'),sx,sw;
 rs.addEventListener('mousedown',function(e){e.stopPropagation();sx=e.clientX;sw=th.offsetWidth;
  var mv=function(e2){th.style.width=(sw+e2.clientX-sx)+'px';lS=-1;render()};
  var up=function(){document.removeEventListener('mousemove',mv);document.removeEventListener('mouseup',up)};
  document.addEventListener('mousemove',mv);document.addEventListener('mouseup',up)})})}
function tsf(b,s){if(sF.has(s)){sF.delete(s);b.classList.remove('on')}else{sF.add(s);b.classList.add('on')}applyF()}
// ---- Spalten-Filter ----
function _escH(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;')}
function _cfVals(ci){var s=new Set();for(var i=0;i<N;i++){var v=D[i][ci];if(v!=null&&v!=='')s.add(String(v))}return Array.from(s).sort()}
function _cfClose(){if(_cfPanel){_cfPanel.remove();_cfPanel=null}_cfCol=-1}
function _cfOpen(ci,btn){
 _cfClose();_cfCol=ci;
 var vals=_cfVals(ci),active=_cf[ci]||null;
 var panel=document.createElement('div');panel.className='cf-panel';
 var items=vals.map(function(v){return'<label class="cf-item"><input type="checkbox" value="'+_escH(v)+'"'+((!active||active.has(v))?' checked':'')+'>'+_escH(v)+'</label>'}).join('');
 panel.innerHTML='<div class="cf-search"><input type="text" placeholder="Suchen..."></div>'+
  '<div class="cf-list"><label class="cf-item cf-all"><input type="checkbox" class="ca"'+(!active?' checked':'')+'>&#x200E;(Alles)</label>'+items+'</div>'+
  '<div class="cf-footer"><button class="cf-cancel">Abbrechen</button><button class="cf-ok">OK</button></div>';
 var r=btn.getBoundingClientRect();
 panel.style.top=(r.bottom+2)+'px';panel.style.left=Math.max(0,r.right-220)+'px';
 document.body.appendChild(panel);_cfPanel=panel;
 var ca=panel.querySelector('.ca');
 var ics=function(){return Array.from(panel.querySelectorAll('.cf-list input[type=checkbox]:not(.ca)'))};
 ca.addEventListener('change',function(){ics().forEach(function(c){c.checked=ca.checked})});
 ics().forEach(function(c){c.addEventListener('change',function(){ca.checked=ics().every(function(x){return x.checked})})});
 panel.querySelector('.cf-search input').addEventListener('input',function(){
  var q=this.value.toLowerCase();
  panel.querySelectorAll('.cf-item:not(.cf-all)').forEach(function(l){l.style.display=l.textContent.toLowerCase().indexOf(q)>=0?'':'none'})});
 panel.querySelector('.cf-ok').addEventListener('click',function(){
  var chk=ics().filter(function(c){return c.checked}).map(function(c){return c.value});
  if(chk.length===vals.length){delete _cf[ci]}else{_cf[ci]=new Set(chk)}
  btn.classList.toggle('active',!!_cf[ci]);
  _cfClose();applyF()});
 panel.querySelector('.cf-cancel').addEventListener('click',_cfClose)}
function initColumnFilters(){
 var FCOLS=[2,11,3,5,8];
 document.querySelectorAll('.th').forEach(function(th){
  var ci=+th.getAttribute('data-c');
  if(FCOLS.indexOf(ci)<0)return;
  var btn=document.createElement('button');
  btn.className='cf-btn';btn.innerHTML='&#9660;';btn.title='Spalte filtern';
  btn.classList.toggle('active',!!_cf[ci]);
  btn.addEventListener('click',function(e){e.stopPropagation();_cfOpen(ci,btn)});
  th.appendChild(btn)});
 document.addEventListener('click',function(e){if(_cfPanel&&!_cfPanel.contains(e.target))_cfClose()})}
function applyF(){F=[];
 var isRx=useRx&&rxObj,plain=(!useRx&&sTm)?sTm.toLowerCase():'',hasSF=MF&&sF.size<SRC.length;
 var cfKeys=Object.keys(_cf);
 for(var i=0;i<N;i++){var r=D[i];
  if(hasSF&&!sF.has(r[8]))continue;
  if(isRx){if(!rxObj.test(r[12]))continue}else if(plain){if(r[12].indexOf(plain)<0)continue}
  var ok=true;for(var k=0;k<cfKeys.length;k++){var ci=+cfKeys[k];if(!_cf[ci].has(String(r[ci]!=null?r[ci]:''))){{ok=false;break}}}
  if(!ok)continue;
  F.push(i)}
 document.getElementById('vc').textContent=F.length+' / '+N;setVH();lS=-1;render()}
function setVH(){vs.style.height=(F.length*RH)+'px'}
function doSort(th){var ci=+th.dataset.c,tp=th.dataset.t;
 if(sC===ci)sD=-sD;else{sC=ci;sD=1}
 thCols.forEach(function(h){h.classList.remove('sa','sd')});th.classList.add(sD===1?'sa':'sd');
 F.sort(function(a,b){var va=D[a][ci],vb=D[b][ci];
  if(tp==='n'){va=parseFloat(va)||0;vb=parseFloat(vb)||0;return(va-vb)*sD}
  return String(va).localeCompare(String(vb))*sD});lS=-1;render()}
var lS=-1,lE=-1;
function render(){var st=tw.scrollTop,vh=tw.clientHeight,thH=document.getElementById('thd').offsetHeight;
 var adj=Math.max(0,st-thH),start=Math.floor(adj/RH),end=Math.min(F.length,start+Math.ceil(vh/RH)+4);
 start=Math.max(0,start-2);if(start===lS&&end===lE)return;lS=start;lE=end;
 var ws=[];thCols.forEach(function(t){ws.push(t.style.display==='none'?0:t.offsetWidth)});
 var h='';
 for(var vi=start;vi<end;vi++){var di=F[vi],r=D[di],y=vi*RH,lv=(r[2]||'').toUpperCase();
  var cls='vr'+(vi%2===0?' ev':' od')+(di===sDI?' sel':'');
  if(lv==='ERROR')cls+=' le';else if(lv==='WARNING')cls+=' lw';
  h+='<div class="'+cls+'" style="top:'+y+'px;height:'+RH+'px" onclick="cR('+vi+','+di+')">';
  for(var ci=0;ci<colOrder.length;ci++){var c=colOrder[ci];if(ws[ci]===0)continue;var v=r[c]!=null?String(r[c]):'';
   if(c===2)v='<mark class="'+lv+'">'+esc(v)+'</mark>';
   else if(c===8&&MF){var sc=srcCls[v]||'';v='<span class="stag '+sc+'">'+esc(v)+'</span>'}
   else if(c===9){var t80=v.length>80?v.substring(0,80)+'...':v;v=esc(t80)}
   else v=esc(v);
   h+='<div class="vc c'+c+'" style="width:'+ws[ci]+'px">'+v+'</div>'}
  h+='</div>'}
 vs.innerHTML=h}
function esc(s){if(!s)return'';return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;')}
function cR(vi,di){if(sDI===di&&document.getElementById('panel').classList.contains('open')){pCl();return}
 sDI=di;sI=vi;lS=-1;render();showD(di)}
function showD(di){var r=D[di],p=document.getElementById('panel');p.classList.add('open');
 var t='Zeile '+r[0]+' \u2014 '+r[1]+' \u2014 '+(r[2]||'');if(MF&&r[8])t+=' \u2014 '+r[8];
 document.getElementById('ptit').textContent=t;
 var h='<div class="ds"><div class="dsh" onclick="tg(this)">\u00DCbersicht</div><div class="dsc"><dl class="dm">';
 h+='<dt>Zeit</dt><dd>'+esc(r[1])+'</dd>';if(r[11])h+='<dt>Prozess</dt><dd>'+esc(r[11])+'</dd>';h+='<dt>Thread</dt><dd>'+esc(r[3])+'</dd>';
 h+='<dt>Level</dt><dd><mark class="'+(r[2]||'').toUpperCase()+'">'+esc(r[2])+'</mark></dd>';
 if(r[4])h+='<dt>Dauer(ms)</dt><dd>'+esc(r[4])+'</dd>';
 h+='<dt>Funktion</dt><dd>'+esc(r[5])+'</dd><dt>Quelle</dt><dd>'+esc(r[7])+'</dd>';
 if(MF&&r[8])h+='<dt>Datei</dt><dd><span class="stag '+(srcCls[r[8]]||'')+'">'+esc(r[8])+'</span></dd>';
 h+='</dl></div></div>';
 h+='<div class="ds"><div class="dsh" onclick="tg(this)">Meldung</div><div class="dsc"><pre>'+esc(r[6])+'</pre></div></div>';
 if(r[9])h+='<div class="ds"><div class="dsh" onclick="tg(this)">Detail</div><div class="dsc"><pre>'+esc(r[9])+'</pre></div></div>';
 if(r[10])h+='<div class="ds"><div class="dsh" onclick="tg(this)">Callstack</div><div class="dsc">'+fCS(r[10])+'</div></div>';
 document.getElementById('pby').innerHTML=h}
function fCS(cs){if(!cs)return'';cs=cs.replace(/[\r\n]+/g,' ').replace(/\s+/g,' ').trim();
 cs=cs.replace(/\s+(?=\d+\s+#\s)/g,'\x00');var p=cs.split('\x00'),h='';
 for(var i=0;i<p.length;i++){var t=p[i].trim();if(!t)continue;
  var m=t.match(/^(\d+)\s+#\s+(\S+)\s+(.+)$/);
  if(m)h+='<div class="csl"><span class="csn">'+m[1]+'</span><span class="csm">'+esc(m[2])+'</span><span class="csf">'+esc(m[3].trim())+'</span></div>';
  else h+='<div class="csl"><span class="csf">'+esc(t)+'</span></div>'}return h}
function tg(el){el.classList.toggle('x');el.nextElementSibling.classList.toggle('x')}
function pCl(){document.getElementById('panel').classList.remove('open');sDI=-1;lS=-1;render()}
function pTg(){var p=document.getElementById('panel');if(p.style.width==='100%'){p.style.width='50%';p.style.left=''}else{p.style.width='100%';p.style.left='0'}}
function pDk(){var p=document.getElementById('panel');p.style.width='50%';p.style.left='';p.style.right='0';p.style.top='0'}
function onKey(e){if((e.ctrlKey||e.metaKey)&&e.key==='f'){e.preventDefault();document.getElementById('si').focus();return}
 if(e.key==='Escape'){pCl();return}
 if(sDI>=0&&document.getElementById('panel').classList.contains('open')){
  if(e.key==='ArrowDown'){e.preventDefault();nv(1)}else if(e.key==='ArrowUp'){e.preventDefault();nv(-1)}}}
function nv(d){var ni=sI+d;if(ni<0||ni>=F.length)return;cR(ni,F[ni]);
 // Zeile ni liegt bei thH+y; der Kopf (sticky) deckt oben thH ab.
 var thH=document.getElementById('thd').offsetHeight,y=ni*RH,st=tw.scrollTop,vh=tw.clientHeight;
 if(y<st)tw.scrollTop=y;else if(thH+y+RH>st+vh)tw.scrollTop=thH+y+RH-vh}
(function(){var hdr=document.getElementById('phdr'),pnl=document.getElementById('panel'),dr=false,sx,sy,ol,ot;
hdr.addEventListener('mousedown',function(e){if(e.target.closest('._pb'))return;dr=true;sx=e.clientX;sy=e.clientY;
 var r=pnl.getBoundingClientRect();ol=r.left;ot=r.top;
 document.addEventListener('mousemove',onD);document.addEventListener('mouseup',stD);e.preventDefault()});
function onD(e){if(!dr)return;pnl.style.left=(ol+e.clientX-sx)+'px';pnl.style.top=(ot+e.clientY-sy)+'px';pnl.style.right='auto'}
function stD(){dr=false;document.removeEventListener('mousemove',onD);document.removeEventListener('mouseup',stD)}
hdr.addEventListener('dblclick',function(e){if(e.target.closest('._pb'))return;pTg()})})();
(function(){if(location.protocol!=='http:'&&location.protocol!=='https:')return;
 fetch('/api/ping').then(function(r){return r.ok?r.text():Promise.reject()}).then(function(){
  document.getElementById('rfBtn').style.display='';document.getElementById('stBtn').style.display='';_startHB()}).catch(function(){})})();
var _reloading=false,_hbTimer=null;
function _startHB(){if(_hbTimer)return;_hbTimer=setInterval(function(){
 fetch('/api/ping').catch(function(){})},10000);
 document.addEventListener('visibilitychange',function(){if(!document.hidden)fetch('/api/ping').catch(function(){})})}
function doRefresh(){_reloading=true;var btn=document.getElementById('rfBtn');btn.classList.add('spin');btn.textContent='\u21BB Lade...';
 fetch('/api/update').then(function(r){return r.json()}).then(function(d){
  if(d.newEntries>0){var _st={t:sTm,rx:useRx,cf:{},sf:MF?Array.from(sF):null};Object.keys(_cf).forEach(function(k){_st.cf[k]=Array.from(_cf[k])});try{sessionStorage.setItem('lgFS',JSON.stringify(_st))}catch(e){}location.reload()}else{_reloading=false;btn.classList.remove('spin');btn.textContent='\u21BB Aktuell ('+N+')';
   setTimeout(function(){btn.textContent='\u21BB Aktualisieren'},2000)}
 }).catch(function(){_reloading=false;btn.classList.remove('spin');btn.textContent='\u21BB Fehler!';setTimeout(function(){btn.textContent='\u21BB Aktualisieren'},3000)})}
function doStop(){if(!confirm('Server beenden?'))return;
 clearInterval(_hbTimer);
 fetch('/api/stop').then(function(){document.getElementById('rfBtn').style.display='none';document.getElementById('stBtn').style.display='none';
  document.title='[Gestoppt] '+document.title}).catch(function(){})}
window.addEventListener('beforeunload',function(){clearInterval(_hbTimer)})
</script>
</body>
</html>
"@

[System.IO.File]::WriteAllText(
    [System.IO.Path]::GetFullPath($HtmlFile), $htmlDoc, [System.Text.UTF8Encoding]::new($false))

$sizeMB = [math]::Round((Get-Item $HtmlFile).Length / 1MB, 1)
$sw.Stop()
Write-Host "Fertig: $HtmlFile ($sizeMB MB) in $([int]$sw.Elapsed.TotalSeconds)s"

if ($Serve) {
    $HtmlFile = [System.IO.Path]::GetFullPath($HtmlFile)
    $LogFile = $LogFile | ForEach-Object { [System.IO.Path]::GetFullPath($_) }

    # Naechsten freien Port ab $Port suchen
    $listener = $null
    $maxPort = $Port + 50
    while ($Port -le $maxPort) {
    $url = "http://localhost:$Port/"
        $candidate = [System.Net.HttpListener]::new()
        $candidate.Prefixes.Add($url)
        try { $candidate.Start(); $listener = $candidate; break }
        catch { $candidate.Close(); $Port++ }
    }
    if (-not $listener) {
        Write-Host "Kein freier Port im Bereich $($Port - 50)-$maxPort gefunden." -ForegroundColor Red
        return
    }
    Write-Host "`n  Log Viewer: $url`n  Beenden mit Ctrl+C oder Tab schliessen`n" -ForegroundColor Cyan

    # URL in Temp-Datei schreiben, damit Debug-Log.cmd den Browser starten kann
    $urlFile = [System.IO.Path]::Combine($env:TEMP, "DebugLog-Serve-Url.txt")
    [System.IO.File]::WriteAllText($urlFile, $url)

    $lastPing = [DateTime]::UtcNow
    $heartbeatTimeout = 120  # Sekunden ohne Ping -> Server beendet sich (erhoeht fuer Hintergrund-Tabs)
    $browserConnected = $false
    try {
        while ($listener.IsListening) {
            # Nicht-blockierend: Async mit Timeout-Pruefung
            $ctxTask = $listener.GetContextAsync()
            while (-not $ctxTask.AsyncWaitHandle.WaitOne(2000)) {
                # Alle 2s pruefen ob Browser noch da ist
                if ($browserConnected -and ([DateTime]::UtcNow - $lastPing).TotalSeconds -gt $heartbeatTimeout) {
                    Write-Host "`n  Browser-Tab geschlossen - Server beendet." -ForegroundColor Yellow
                    $listener.Stop()
                    return
                }
            }
            $ctx = $ctxTask.GetAwaiter().GetResult()
            $req = $ctx.Request; $res = $ctx.Response; $path = $req.Url.AbsolutePath
            try {
                if ($path -eq "/api/ping") {
                    $lastPing = [DateTime]::UtcNow
                    $browserConnected = $true
                    $body = [System.Text.Encoding]::UTF8.GetBytes("ok")
                    $res.ContentType = "text/plain"; $res.ContentLength64 = $body.Length
                    $res.OutputStream.Write($body, 0, $body.Length)
                } elseif ($path -eq "/api/stop") {
                    Write-Host "`n  Server wird beendet..." -ForegroundColor Yellow
                    $body = [System.Text.Encoding]::UTF8.GetBytes("{`"stopped`":true}")
                    $res.ContentType = "application/json"; $res.ContentLength64 = $body.Length
                    $res.OutputStream.Write($body, 0, $body.Length)
                    $res.OutputStream.Close()
                    break
                } elseif ($path -eq "/api/update") {
                    $lastPing = [DateTime]::UtcNow  # Timer reset: Update-Verarbeitung zaehlt nicht als Ausfall
                    Write-Host "[$(Get-Date -f 'HH:mm:ss')] Update..."
                    $fsH = [System.IO.FileStream]::new($HtmlFile, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
                    $hBuf = New-Object byte[] ([Math]::Min(2000, $fsH.Length))
                    [void]$fsH.Read($hBuf, 0, $hBuf.Length); $fsH.Close()
                    $metaM = [regex]::Match([System.Text.Encoding]::UTF8.GetString($hBuf), 'LOGMETA:entries=(\d+)')
                    $existCount = if ($metaM.Success) { [int]$metaM.Groups[1].Value } else { 0 }
                    $allE = @()
                    foreach ($lf in $LogFile) {
                        $encU = ResolveEncoding $lf; $cU = ReadLogFile $lf $encU
                        $cU = LimitLines $cU
                        $feU = $cU | ConvertFrom-Csv -Delimiter "`t"; $tagU = MakeFileTag $lf
                        foreach ($e in $feU) { Add-Member -InputObject $e -NotePropertyName '_Tag' -NotePropertyValue $tagU -Force }
                        $allE += $feU }
                    if ($multiFile) { $allE = $allE | Sort-Object { $_.Zeit } }
                    $logTotal = $allE.Count; $newCount = $logTotal - $existCount
                    if ($newCount -gt 0) {
                        Write-Host "  $newCount neue Eintraege"
                        $curHtml = [System.IO.File]::ReadAllText($HtmlFile, [System.Text.UTF8Encoding]::new($false))
                        $sbU = [System.Text.StringBuilder]::new(2MB)
                        $lvlU = @{ "ERROR"=0; "WARNING"=0; "INFO"=0; "DEBUG"=0; "TRACE"=0 }
                        # Badge-Counts lesen: CSS ist ~7500 Zeichen, Badges bei ~7800
                        $hC = $curHtml.Substring(0, [Math]::Min(12000, $curHtml.Length))
                        foreach ($lv in @("ERROR","WARNING","INFO","DEBUG","TRACE")) {
                            $sM = [regex]::Match($hC, "${lv}:\s*(\d+)"); if ($sM.Success) { $lvlU[$lv] = [int]$sM.Groups[1].Value } }
                        Write-Host "  Bestehende Counts: E=$($lvlU['ERROR']) W=$($lvlU['WARNING']) I=$($lvlU['INFO']) D=$($lvlU['DEBUG']) T=$($lvlU['TRACE'])"
                        for ($j = $existCount; $j -lt $logTotal; $j++) {
                            $eU = $allE[$j]; $tagU = if ($eU._Tag) { $eU._Tag } else { "" }
                            $lvU = if ($eU.LogLevel) { $eU.LogLevel.ToUpper() } else { "" }
                            if ($lvlU.ContainsKey($lvU)) { $lvlU[$lvU]++ }
                            AppendEntryJson $sbU $eU ($j+2) $tagU $true }
                        Write-Host "  JSON fuer neue Eintraege: $($sbU.Length) Zeichen"
                        $js = $curHtml.IndexOf('var D=['); $nm = $curHtml.IndexOf('var N=D.length')
                        $jSrc = $curHtml.IndexOf('var SRC=', $js)
                        $je = if ($jSrc -gt 0) { $curHtml.LastIndexOf('];', $jSrc) } else { $curHtml.LastIndexOf('];', $nm) }
                        Write-Host "  Splice-Marker: D=[$js] ]; =$je N=$nm"
                        if ($js -gt 0 -and $je -gt 0) {
                            $bef = $curHtml.Substring(0, $je); $aft = $curHtml.Substring($je + 1)
                            $newJsonStr = $sbU.ToString()
                            if ($bef.TrimEnd().EndsWith('[')) { $newJsonStr = $newJsonStr.TrimStart(',') }
                            $mg = [System.Text.StringBuilder]::new($bef.Length + $newJsonStr.Length + $aft.Length + 100)
                            [void]$mg.Append($bef)
                            [void]$mg.Append($newJsonStr)
                            [void]$mg.Append(']')
                            [void]$mg.Append($aft)
                            $uH = $mg.ToString()
                            Write-Host "  HTML gesamt: $($uH.Length) Zeichen"
                            $uH = $uH.Replace("LOGMETA:entries=$existCount", "LOGMETA:entries=$logTotal")
                            $uH = [regex]::Replace($uH, '<span class="stot">[^<]+</span>', "<span class=`"stot`">$logTotal Eintr.</span>")
                            $nB = ""
                            if ($lvlU["ERROR"] -gt 0)   { $nB += "<span class='sb e'>ERROR: $($lvlU['ERROR'])</span>" }
                            if ($lvlU["WARNING"] -gt 0)  { $nB += "<span class='sb w'>WARNING: $($lvlU['WARNING'])</span>" }
                            if ($lvlU["INFO"] -gt 0)     { $nB += "<span class='sb i'>INFO: $($lvlU['INFO'])</span>" }
                            if ($lvlU["DEBUG"] -gt 0)    { $nB += "<span class='sb d'>DEBUG: $($lvlU['DEBUG'])</span>" }
                            if ($lvlU["TRACE"] -gt 0)    { $nB += "<span class='sb t'>TRACE: $($lvlU['TRACE'])</span>" }
                            $bS = $uH.IndexOf('<span class="stot">'); $bE = $uH.IndexOf('</div>', $bS)
                            if ($bS -gt 0 -and $bE -gt $bS) { $sE = $uH.IndexOf('</span>', $bS) + 7; $uH = $uH.Substring(0, $sE) + $nB + $uH.Substring($bE) }
                            $lastE = $allE[$logTotal - 1]
                            if ($lastE.Zeit) { $uH = [regex]::Replace($uH, '&#8211;\s*[\d:.]+\s*&nbsp;', "&#8211; $($lastE.Zeit) &nbsp;") }
                            [System.IO.File]::WriteAllText($HtmlFile, $uH, [System.Text.UTF8Encoding]::new($false))
                            Write-Host "  HTML: $logTotal Eintraege" }
                    } else { Write-Host "  Aktuell." }
                    $jr = "{`"newEntries`":$newCount,`"total`":$logTotal}"
                    $body = [System.Text.Encoding]::UTF8.GetBytes($jr)
                    $res.ContentType = "application/json"; $res.ContentLength64 = $body.Length
                    $res.OutputStream.Write($body, 0, $body.Length)
                } else {
                    $lastPing = [DateTime]::UtcNow; $browserConnected = $true
                    if ($path -eq '/') { Write-Host "  Seite geladen (Reload)." }
                    $hb = [System.IO.File]::ReadAllBytes($HtmlFile)
                    $res.ContentType = "text/html; charset=utf-8"; $res.ContentLength64 = $hb.Length
                    $res.OutputStream.Write($hb, 0, $hb.Length) }
            } catch { Write-Host "  Fehler: $_" -ForegroundColor Red
                $eb = [System.Text.Encoding]::UTF8.GetBytes("{`"error`":`"$_`"}")
                $res.StatusCode = 500; $res.ContentType = "application/json"; $res.ContentLength64 = $eb.Length
                $res.OutputStream.Write($eb, 0, $eb.Length)
            } finally { $res.OutputStream.Close() }
        }
    } finally { $listener.Stop(); Write-Host "Server gestoppt." }
}
