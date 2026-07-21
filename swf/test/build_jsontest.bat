@echo off
rem Builds the offline MCM JSON sanitizer validator (jsontest.exe).
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cl /nologo /std:c++20 /O2 /EHsc /MD /W3 ^
   /I"..\..\include" ^
   /I"..\..\build\release\vcpkg_installed\x64-windows-static-md\include" ^
   jsontest.cpp ^
   /Fe:jsontest.exe
