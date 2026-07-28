@echo off
rem Generates/updates an English translation file (en.json) from the
rem Translate("...") calls in a plugin's C++ source tree.
rem
rem Usage:
rem   extract_translations.bat <sourceDir> <path\to\en.json>
rem Example:
rem   extract_translations.bat ..\MyPlugin\src ..\MyPlugin\resources\Languages\en.json
if "%~2"=="" (
    echo Usage: %~nx0 ^<sourceDir^> ^<path\to\en.json^>
    exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0extract_translations.ps1" -SourceDir "%~1" -OutJson "%~2"
