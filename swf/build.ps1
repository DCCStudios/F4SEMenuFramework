# Builds F4SEFramework.swf (the pause-menu button SWF) from src/F4SEFrameworkPause.as.
#
# Clean-room, self-contained SWF: one AS3 document class, no MCM code, no Flex
# framework. Compiled with the Apache Flex SDK's mxmlc against the Adobe AIR
# SDK's playerglobal.swc (an empty -load-config avoids linking the Flex
# framework, keeping the output ~1 KB).
#
# Toolchain (downloaded once, outside the repo):
#   Apache Flex SDK 4.16.1  -> E:\Fallout 4 Modding\F4SE\_tools\flex
#   Adobe AIR SDK 32.0      -> E:\Fallout 4 Modding\F4SE\_tools\airsdk (playerglobal.swc)
#   Requires Java 8+ on PATH.

$ErrorActionPreference = "Stop"

$work  = Split-Path -Parent $MyInvocation.MyCommand.Path
$flex  = "E:\Fallout 4 Modding\F4SE\_tools\flex"
$pg    = "E:\Fallout 4 Modding\F4SE\_tools\airsdk\frameworks\libs\player\32.0\playerglobal.swc"
$src   = Join-Path $work "src\F4SEFrameworkPause.as"
$out   = Join-Path $work "F4SEFramework.swf"

java -jar "$flex\lib\mxmlc.jar" `
    +flexlib="$flex\frameworks" `
    "-load-config=" `
    "-external-library-path=$pg" `
    "-swf-version=32" `
    "-source-path=$work\src" `
    "-output=$out" `
    "$src"

if (Test-Path $out) {
    Write-Output ("Built: {0} ({1} bytes)" -f $out, (Get-Item $out).Length)

    # Deploy into the mod's Interface folder (virtualises to Data/Interface).
    $modInterface = "F:\Modlists\LoreOut\mods\F4SE Menu Framework\Interface"
    if (Test-Path (Split-Path $modInterface -Parent)) {
        New-Item -ItemType Directory -Force -Path $modInterface | Out-Null
        Copy-Item $out (Join-Path $modInterface "F4SEFramework.swf") -Force
        Write-Output ("Deployed to: {0}" -f $modInterface)
    }
} else {
    throw "SWF build failed."
}
