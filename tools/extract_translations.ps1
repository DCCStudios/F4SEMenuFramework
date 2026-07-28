# extract_translations.ps1
#
# Scans a C++ source tree for F4SE Menu Framework Translate() calls with
# string-literal keys and merges the keys into a flat en.json translation
# file (value = key for new entries, existing values are never overwritten).
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File extract_translations.ps1 -SourceDir <dir> -OutJson <path\to\en.json>
# or via the companion extract_translations.bat.
#
# Recognized call shapes (whitespace-tolerant):
#   Translate("Key")
#   Translate("PluginName", "Key")          (the second literal is the key)
#   F4SEMenuFramework::Translate("Key")
#
# Known limits (documented in PLUGIN_DEVELOPMENT_GUIDE.md):
#   - Dynamic keys (Translate(variable)) cannot be extracted.
#   - Commented-out Translate calls still match; prune those entries by hand.
#
# Compatible with Windows PowerShell 5.1 (no -AsHashtable, no ternary).

param(
    [Parameter(Mandatory = $true)][string]$SourceDir,
    [Parameter(Mandatory = $true)][string]$OutJson
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $SourceDir -PathType Container)) {
    Write-Error "Source directory not found: $SourceDir"
    exit 1
}

# Turns a C++ string literal body back into the runtime string.
function Unescape-CppLiteral([string]$s) {
    $sb = New-Object System.Text.StringBuilder
    $i = 0
    while ($i -lt $s.Length) {
        $c = $s[$i]
        if ($c -eq '\' -and $i + 1 -lt $s.Length) {
            $n = $s[$i + 1]
            if ($n -eq '"') { [void]$sb.Append('"');  $i += 2 }
            elseif ($n -eq '\') { [void]$sb.Append('\');  $i += 2 }
            elseif ($n -eq 'n') { [void]$sb.Append("`n"); $i += 2 }
            elseif ($n -eq 't') { [void]$sb.Append("`t"); $i += 2 }
            elseif ($n -eq 'r') { [void]$sb.Append("`r"); $i += 2 }
            else { [void]$sb.Append($c); $i += 1 }  # unknown escape: keep the backslash
        } else {
            [void]$sb.Append($c)
            $i += 1
        }
    }
    return $sb.ToString()
}

# One or two string-literal arguments; when two are present the key is the
# second (explicit pluginName overload).
$lit = '"((?:[^"\\]|\\.)*)"'
$callRegex = [regex]("\bTranslate\s*\(\s*$lit\s*(?:,\s*$lit\s*)?\)")

$foundKeys = New-Object System.Collections.Generic.HashSet[string]

$files = Get-ChildItem -LiteralPath $SourceDir -Recurse -File -Include *.cpp, *.h, *.hpp, *.cxx, *.cc, *.inl
foreach ($file in $files) {
    $text = [System.IO.File]::ReadAllText($file.FullName)
    foreach ($m in $callRegex.Matches($text)) {
        if ($m.Groups[2].Success) {
            $raw = $m.Groups[2].Value   # Translate("Plugin", "Key")
        } else {
            $raw = $m.Groups[1].Value   # Translate("Key")
        }
        $key = Unescape-CppLiteral $raw
        if ($key.Length -gt 0) { [void]$foundKeys.Add($key) }
    }
}

Write-Host ("Scanned {0} file(s), found {1} unique Translate() key(s)." -f $files.Count, $foundKeys.Count)

# Load the existing en.json (if any) into an ordered map. PS 5.1 lacks
# ConvertFrom-Json -AsHashtable, so enumerate the PSCustomObject properties.
$existing = [ordered]@{}
if (Test-Path -LiteralPath $OutJson -PathType Leaf) {
    $jsonText = [System.IO.File]::ReadAllText($OutJson)
    if ($jsonText.Trim().Length -gt 0) {
        $obj = $jsonText | ConvertFrom-Json
        foreach ($prop in $obj.PSObject.Properties) {
            $existing[$prop.Name] = [string]$prop.Value
        }
    }
}

# Merge: new keys get themselves as the value (complete for the
# English-text-as-key convention, a fill-in skeleton for the ID convention).
$added = 0
foreach ($key in ($foundKeys | Sort-Object)) {
    if (-not $existing.Contains($key)) {
        $existing[$key] = $key
        $added += 1
    }
}

# Report entries that no longer appear in the source (kept, not deleted:
# they may be dynamic keys or belong to another source tree).
$stale = @()
foreach ($key in $existing.Keys) {
    if (-not $foundKeys.Contains($key)) { $stale += $key }
}

# Emit sorted, pretty-printed JSON without a BOM. Hand-rolled writer keeps
# key order stable and avoids ConvertTo-Json's \uXXXX noise for non-ASCII.
function Escape-Json([string]$s) {
    $sb = New-Object System.Text.StringBuilder
    foreach ($ch in $s.ToCharArray()) {
        switch ($ch) {
            '"'  { [void]$sb.Append('\"') }
            '\'  { [void]$sb.Append('\\') }
            "`n" { [void]$sb.Append('\n') }
            "`r" { [void]$sb.Append('\r') }
            "`t" { [void]$sb.Append('\t') }
            default {
                if ([int]$ch -lt 0x20) {
                    [void]$sb.Append('\u{0:x4}' -f [int]$ch)
                } else {
                    [void]$sb.Append($ch)
                }
            }
        }
    }
    return $sb.ToString()
}

$sortedKeys = @($existing.Keys | Sort-Object)
$lines = New-Object System.Collections.Generic.List[string]
[void]$lines.Add('{')
for ($i = 0; $i -lt $sortedKeys.Count; $i++) {
    $k = $sortedKeys[$i]
    $comma = ','
    if ($i -eq $sortedKeys.Count - 1) { $comma = '' }
    [void]$lines.Add(('  "{0}": "{1}"{2}' -f (Escape-Json $k), (Escape-Json $existing[$k]), $comma))
}
[void]$lines.Add('}')

$outDir = Split-Path -Parent $OutJson
if ($outDir -and -not (Test-Path -LiteralPath $outDir)) {
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null
}
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($OutJson, (($lines -join "`r`n") + "`r`n"), $utf8NoBom)

Write-Host ("Wrote {0}: {1} key(s) total, {2} newly added." -f $OutJson, $sortedKeys.Count, $added)
if ($stale.Count -gt 0) {
    Write-Host ""
    Write-Host "Entries not found in the source (kept; remove by hand if obsolete):"
    foreach ($k in $stale) { Write-Host ("  - {0}" -f $k) }
}
