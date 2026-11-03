@echo off
setlocal
rem Builds a portable copy of the Ghoulies PC port, ready to copy to a USB stick.
rem Output: G:\xboxrecomp\GhouliesPortable  (the original folders are only read, never changed)
set SRC=G:\xboxrecomp\ghoulies_recomp
set OUT=G:\xboxrecomp\GhouliesPortable

if not exist "%SRC%\ghoulies\build64\Release\ghoulies_recomp.exe" (
  echo No build found - ask Claude for a build first.
  pause & exit /b 1
)
echo Making the portable copy in %OUT% ...
if not exist "%OUT%" mkdir "%OUT%"

rem the program, its shader cache and the two settings files
copy /y "%SRC%\ghoulies\build64\Release\ghoulies_recomp.exe" "%OUT%\" >nul || goto fail
if exist "%SRC%\ghoulies\build64\Release\shadercache" robocopy "%SRC%\ghoulies\build64\Release\shadercache" "%OUT%\shadercache" /e /njh /njs /nfl /ndl /np >nul
copy /y "%SRC%\ghoulies\portable\engine.ini" "%OUT%\" >nul || goto fail
if not exist "%OUT%\settings.ini" copy /y "%SRC%\ghoulies\portable\settings.ini" "%OUT%\" >nul
copy /y "%SRC%\ghoulies\portable\Play.bat" "%OUT%\" >nul
copy /y "%SRC%\ghoulies\portable\README.txt" "%OUT%\" >nul

rem the Visual C++ runtime, so the laptop does not need it installed
copy /y "%SystemRoot%\System32\vcruntime140.dll" "%OUT%\" >nul || goto fail
copy /y "%SystemRoot%\System32\vcruntime140_1.dll" "%OUT%\" >nul || goto fail

rem the game's own files, with your saves (UDATA); not the port's working files
robocopy "%SRC%\xboxrecomp-main\game_files" "%OUT%\game_files" /e /xf *.json /xd "Other PNGs" /njh /njs /nfl /ndl
if errorlevel 8 goto fail

echo.
echo Done. Copy the whole GhouliesPortable folder to the USB stick,
echo then on the laptop double-click Play.bat inside it.
explorer "%OUT%"
pause
exit /b 0

:fail
echo Something failed while copying - see above.
pause
exit /b 1
