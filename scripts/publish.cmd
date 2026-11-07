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

rem Clean previous output to avoid stale files.
rem NOTE: since v0.5.2 user data lives in %LOCALAPPDATA%\Orua, NOT in dist\Orua\data,
rem so rebuilding dist\Orua no longer destroys app.db / uploads (avatars) / config.json.
rem If this machine still has the old layout, migrate it out first to avoid data loss.
if exist "dist\Orua\data" call :migrate_legacy
if errorlevel 1 exit /b 1
if exist "dist\Orua" rmdir /s /q "dist\Orua"

"%DOTNETEXE%" publish src\AiCompanion.App\AiCompanion.App.csproj -c Release -r win-x64 --self-contained false -o dist\Orua
if errorlevel 1 exit /b 1

rem deploy the backend beside the app exe (production layout: backend\backend.exe)
if not exist "dist\Orua\backend" mkdir "dist\Orua\backend"
copy /y bin\backend\backend.exe "dist\Orua\backend\" >nul
if errorlevel 1 exit /b 1

rem create desktop shortcut pointing at the published exe
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ws = New-Object -ComObject WScript.Shell; $lnk = $ws.CreateShortcut((Join-Path ([Environment]::GetFolderPath('Desktop')) 'Orua.lnk')); $lnk.TargetPath = (Resolve-Path 'dist\Orua\AiCompanion.exe').Path; $lnk.WorkingDirectory = (Resolve-Path 'dist\Orua').Path; $lnk.Description = 'Orua'; $lnk.Save()"
if errorlevel 1 exit /b 1

echo [OK] Published to dist\Orua. Desktop shortcut created: Orua.lnk
endlocal
exit /b 0

:migrate_legacy
echo [INFO] migrating legacy dist\Orua\data to %LOCALAPPDATA%\Orua ...
if not exist "%LOCALAPPDATA%\Orua" mkdir "%LOCALAPPDATA%\Orua"
robocopy "dist\Orua\data" "%LOCALAPPDATA%\Orua" /E /XC /XN /XO /NFL /NDL /NJH /NJS >nul
rem robocopy exit codes: 0-7 are success, 8+ indicate real failures
if errorlevel 8 (
  echo [ERROR] failed to migrate legacy data. Abort to avoid data loss.
  exit /b 1
)
echo [INFO] legacy data migrated.
exit /b 0
