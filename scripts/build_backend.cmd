@echo off
setlocal
cd /d "%~dp0.."

rem ---- locate CMake/CTest (fall back to the Visual Studio install dir) ----
set "CMAKEEXE=cmake"
set "CTESTEXE=ctest"
where cmake >nul 2>&1
if not errorlevel 1 goto cmake_ready
set "VSDIR="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto cmake_missing
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto cmake_missing
set "CMAKEEXE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "CTESTEXE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
if not exist "%CMAKEEXE%" goto cmake_missing

:cmake_ready
"%CMAKEEXE%" -S backend -B backend\build -A x64
if errorlevel 1 exit /b 1
"%CMAKEEXE%" --build backend\build --config Release
if errorlevel 1 exit /b 1
"%CTESTEXE%" --test-dir backend\build -C Release --output-on-failure
if errorlevel 1 exit /b 1

if not exist bin\backend mkdir bin\backend
copy /y backend\build\Release\backend.exe bin\backend\ >nul
copy /y backend\build\Release\backend_tests.exe bin\backend\ >nul
echo [OK] backend built and tested
goto :eof

:cmake_missing
echo [ERROR] cmake not found. Install VS Build Tools with the CMake component, or add CMake to PATH.
exit /b 1
