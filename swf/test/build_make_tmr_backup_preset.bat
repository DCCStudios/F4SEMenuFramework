@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
cl /nologo /std:c++20 /EHsc /O2 /MD /D_CRT_SECURE_NO_WARNINGS /utf-8 ^
   /I "..\..\include" ^
   make_tmr_backup_preset.cpp ..\..\src\MCM\M8rIniJson.cpp ^
   /Fe:make_tmr_backup_preset.exe
if errorlevel 1 exit /b 1
set OUT=f:\Modlists\TMR\mods\MCM Settings Manager\MCM\Settings\Presets\TMRCurrentBackup.ini
make_tmr_backup_preset.exe "%OUT%"
if errorlevel 1 exit /b 1
mkdir "f:\Modlists\TMR\overwrite\MCM\Settings\Presets" 2>nul
copy /Y "%OUT%" "f:\Modlists\TMR\overwrite\MCM\Settings\Presets\TMRCurrentBackup.ini" >nul
echo also copied to overwrite
dir "%OUT%"
