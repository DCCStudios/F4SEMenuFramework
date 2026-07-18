@echo off
rem Builds the standalone SWF rasterizer validator (swftest.exe).
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cl /nologo /std:c++20 /O2 /EHsc /MD /W3 ^
   /I"..\..\include" ^
   /I"..\..\build\release\vcpkg_installed\x64-windows-static-md\include" ^
   swftest.cpp ^
   "..\..\src\MCM\SWFParseCommon.cpp" ^
   "..\..\src\MCM\SWFLibraryImage.cpp" ^
   "..\..\src\MCM\SWFVectorMovie.cpp" ^
   /Fe:swftest.exe ^
   /link windowscodecs.lib ole32.lib "..\..\build\release\vcpkg_installed\x64-windows-static-md\lib\zlib.lib"
