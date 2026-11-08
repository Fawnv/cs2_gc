@echo off
rem Starts CS2 with cs2_gc (always -insecure, VAC off) and cleans up after it.
rem Normal CS2 is unaffected: launch it from Steam as usual.
rem
rem cs2_gc.exe has to sit next to cs2.exe to start the engine, so it is copied
rem there only for this session and removed (with any crash dumps) when the game
rem closes. All gc data (config, inventory, log, editor) stays in release\cs2_gc.
rem Keep this window open while playing. If it gets closed early, run
rem cleanup_cs2_folder.cmd afterwards.
rem Extra launch options are passed through, e.g.  run_cs2_gc.cmd -novid -console
setlocal

rem edit if CS2 lives somewhere else (cleanup_cs2_folder.cmd uses the same value)
set "CS2_ROOT=C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive"
set "CS2_BIN=%CS2_ROOT%\game\bin\win64"
set "GC_DATA=%~dp0release\cs2_gc"

if not exist "%CS2_BIN%\cs2.exe" (
    echo CS2 not found in "%CS2_BIN%". Edit CS2_BIN at the top of this file.
    pause
    exit /b 1
)

copy /Y "%~dp0release\cs2_gc.exe" "%CS2_BIN%\cs2_gc.exe" >nul || (
    echo Could not copy cs2_gc.exe next to cs2.exe.
    pause
    exit /b 1
)

echo CS2 with cs2_gc is running. Inventory editor: http://127.0.0.1:13001/
echo Do not close this window: cs2_gc files are removed from the CS2 folder when the game exits.
start "" /wait "%CS2_BIN%\cs2_gc.exe" -insecure -gcdata "%GC_DATA%" %*

call "%~dp0cleanup_cs2_folder.cmd" /quiet
