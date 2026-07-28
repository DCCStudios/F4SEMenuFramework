@echo off
rem Rebuilds the offline plugin localization validator (loctest.exe).
rem PluginLocalization.cpp and MCMTranslation.cpp are CommonLibF4-free and
rem logger-free by design so they compile standalone here.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
cl /nologo /std:c++20 /EHsc /O2 /MD /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /utf-8 ^
   /I "..\..\include" ^
   /I "..\..\build\release\vcpkg_installed\x64-windows-static-md\include" ^
   loctest.cpp ..\..\src\PluginLocalization.cpp ..\..\src\MCM\MCMTranslation.cpp ^
   /Fe:loctest.exe /link shell32.lib
