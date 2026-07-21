@echo off
rem Rebuilds the offline MCM translation validator (trtest.exe).
rem Compiles MCMTranslation.cpp standalone (it is CommonLibF4-free by design).
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
cl /nologo /std:c++20 /EHsc /O2 /MD /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
   /I "..\..\include" ^
   trtest.cpp ..\..\src\MCM\MCMTranslation.cpp ^
   /Fe:trtest.exe /link shell32.lib
