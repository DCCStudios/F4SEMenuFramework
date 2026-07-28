@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
cl /nologo /std:c++20 /EHsc /O2 /MD /D_CRT_SECURE_NO_WARNINGS /utf-8 ^
   /I "..\..\include" ^
   parse_preset_check.cpp ..\..\src\MCM\M8rIniJson.cpp ^
   /Fe:parse_preset_check.exe
if errorlevel 1 exit /b 1
parse_preset_check.exe "f:\Modlists\TMR\mods\MCM Settings Manager\MCM\Settings\Presets\FrameworkTestPreset.ini"
