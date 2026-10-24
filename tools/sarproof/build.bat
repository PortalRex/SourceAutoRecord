@echo off
rem Builds sarproof.exe (32-bit, like SAR) with the Visual Studio 2019 C++ tools.
setlocal
set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
call "%VCVARS%" x86 >nul || exit /b 1
set "HERE=%~dp0"
set "SRC=%HERE%..\..\src"
if not exist "%HERE%obj" mkdir "%HERE%obj"
cl /nologo /std:c++17 /O2 /EHsc /MT /W3 /D_CRT_SECURE_NO_WARNINGS /I"%SRC%" /Fo"%HERE%obj\\" /Fe"%HERE%sarproof.exe" ^
	"%HERE%main.cpp" ^
	"%SRC%\Features\Demo\ProofFile.cpp" ^
	"%SRC%\Features\Demo\ProofConvert.cpp" ^
	"%SRC%\Utils\miniz\miniz.cpp" ^
	"%SRC%\Utils\ed25519\*.cpp" ^
	advapi32.lib
