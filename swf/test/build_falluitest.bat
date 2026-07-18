@echo off
rem Rebuilds the offline FallUI HUD editor validator (falluitest.exe).
rem Reuses the prebuilt MCMTranslation/imgui .obj files in this folder.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
cl /nologo /std:c++20 /EHsc /O2 /MD /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
   /I fakes /I "..\..\include" /I "..\..\src" /I "..\..\imgui" ^
   falluitest.cpp MCMTranslation.obj imgui.obj imgui_draw.obj imgui_tables.obj imgui_widgets.obj ^
   /Fe:falluitest.exe
