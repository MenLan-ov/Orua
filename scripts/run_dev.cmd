@echo off
setlocal
cd /d "%~dp0.."

call scripts\build_backend.cmd
if errorlevel 1 exit /b 1

rem ---- locate dotnet (fall back to the default install path) ----
set "DOTNETEXE=dotnet"
where dotnet >nul 2>&1
if errorlevel 1 (
  if exist "%ProgramFiles%\dotnet\dotnet.exe" set "DOTNETEXE=%ProgramFiles%\dotnet\dotnet.exe"
)
"%DOTNETEXE%" --version >nul 2>&1
if errorlevel 1 (
  echo [ERROR] .NET SDK not found. Install the .NET 8 SDK first.
  exit /b 1
)

"%DOTNETEXE%" build AiCompanion.sln -c Debug
if errorlevel 1 exit /b 1

rem deploy the backend next to the WPF output (production layout: backend\backend.exe beside app.exe)
set APPDIR=src\AiCompanion.App\bin\Debug\net8.0-windows
if not exist "%APPDIR%\backend" mkdir "%APPDIR%\backend"
copy /y bin\backend\backend.exe "%APPDIR%\backend\" >nul

rem dev mode: run "set LLM_MOCK=1" first to use the built-in mock stream;
rem otherwise fill in a real API in the settings panel
set AI_COMPANION_DATA=%CD%\data
set AI_COMPANION_BACKEND=%CD%\bin\backend\backend.exe

echo Starting desktop app - the backend process is cleaned up on exit...
"%DOTNETEXE%" run --project src\AiCompanion.App -c Debug --no-build

taskkill /im backend.exe /f >nul 2>&1
endlocal
