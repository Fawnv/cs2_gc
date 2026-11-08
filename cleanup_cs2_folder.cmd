@echo off
rem Removes every cs2_gc trace from the CS2 install folder:
rem   game\bin\win64\cs2_gc.exe          (the launcher copy)
rem   game\bin\win64\cs2_gc\             (data folder, only if it was started without -gcdata)
rem   game\bin\win64\cs2_gc_*.mdmp       (crash dumps the game names after the exe)
rem cs2_gc never modifies original game files, the registry or Steam launch options,
rem so after this the folder is exactly what Steam installed.
rem run_cs2_gc.cmd calls this automatically when the game exits.
setlocal

rem edit if CS2 lives somewhere else (or set CS2_ROOT before calling)
if not defined CS2_ROOT set "CS2_ROOT=C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive"
set "CS2_BIN=%CS2_ROOT%\game\bin\win64"

tasklist /FI "IMAGENAME eq cs2_gc.exe" 2>nul | find /I "cs2_gc.exe" >nul && (
    echo CS2 with cs2_gc is still running. Close the game first.
    if /I not "%~1"=="/quiet" pause
    exit /b 1
)

if exist "%CS2_BIN%\cs2_gc.exe" del /F /Q "%CS2_BIN%\cs2_gc.exe"
if exist "%CS2_BIN%\cs2_gc\" rmdir /S /Q "%CS2_BIN%\cs2_gc"

rem crash dumps are kept for debugging, moved out of the game folder next to the data
if exist "%CS2_BIN%\cs2_gc_*.mdmp" (
    if not exist "%~dp0release\cs2_gc\crashes\" mkdir "%~dp0release\cs2_gc\crashes"
    move /Y "%CS2_BIN%\cs2_gc_*.mdmp" "%~dp0release\cs2_gc\crashes\" >nul
    echo crash dump moved to %~dp0release\cs2_gc\crashes
)
del /F /Q "%CS2_BIN%\cs2_gc_*.mdmp" 2>nul

rem anything else named cs2_gc* anywhere in the install
set "LEFT="
for /f "delims=" %%F in ('dir /S /B "%CS2_ROOT%\cs2_gc*" 2^>nul') do (
    echo still there: %%F
    set "LEFT=1"
)

if defined LEFT (
    echo Some cs2_gc files could not be removed, see above.
    if /I not "%~1"=="/quiet" pause
    exit /b 1
)

echo CS2 folder is clean: no cs2_gc files left.
if /I not "%~1"=="/quiet" pause
