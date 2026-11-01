@echo off
rem Builds koncepcja.exe with MSYS2 MINGW64. Set MSYS2_ROOT if not in C:\msys64.
rem Optional argument: make target (e.g. distrib).
setlocal
if "%MSYS2_ROOT%"=="" set "MSYS2_ROOT=C:\msys64"
if not exist "%MSYS2_ROOT%\msys2_shell.cmd" (
  echo MSYS2 not found at %MSYS2_ROOT%. Install it or set MSYS2_ROOT.
  exit /b 1
)
set "MSYSTEM=MINGW64"
set "CHERE_INVOKING=1"
"%MSYS2_ROOT%\usr\bin\bash.exe" -lc "cd \"$(cygpath '%~dp0..')\" && make ARCH=win64 -j$(nproc) %*"
exit /b %ERRORLEVEL%
