@echo off
rem Rebuilds the offline M8rIniJson codec validator (m8rjsontest.exe).
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
cl /nologo /std:c++20 /EHsc /O2 /MD /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /utf-8 ^
   /I "..\..\include" ^
   m8rjsontest.cpp "..\..\src\MCM\M8rIniJson.cpp" ^
   /Fe:m8rjsontest.exe
