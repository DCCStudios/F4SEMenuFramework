@echo off
rem Rebuilds the offline font-atlas overflow validator (fonttest.exe).
rem Compiles the vendored ImGui core standalone (no backend, no game deps).
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
cl /nologo /std:c++20 /EHsc /O2 /MD /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
   /I "..\..\imgui" ^
   fonttest.cpp ..\..\imgui\imgui.cpp ..\..\imgui\imgui_draw.cpp ^
   ..\..\imgui\imgui_tables.cpp ..\..\imgui\imgui_widgets.cpp ^
   /Fe:fonttest.exe
