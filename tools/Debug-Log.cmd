@echo off
echo %~n0 %*

if "%~1"=="" echo Keine Log-Datei & timeout -t 10 & goto:eof

set dr=%~dp0
set dr=%dr:~0,2%
if "%dr%"=="\\" goto:doItFromShare %*
if "%dr%"=="\\" goto:eof

call:doIt %*
goto:eof

:doIt <logFile> <logFile>
echo %*

set "URLFILE=%temp%\DebugLog-Serve-Url.txt"
if exist "%URLFILE%" del "%URLFILE%"

echo pwsh -NoProfile -File %~dpn0.ps1 -Serve %*
start "Debug-Log-Server" pwsh -NoProfile -File "%~dpn0.ps1" -Serve %*

:waitUrl
timeout /t 1 /nobreak >nul
if not exist "%URLFILE%" goto waitUrl

set /p SERVE_URL=<"%URLFILE%"
del "%URLFILE%"
echo Browser: %SERVE_URL%
start "" "%SERVE_URL%"
goto:eof

:doItFromShare
REM pause
set dr=%~dp0
if "%dr:~-1%"=="\" set dr=%dr:~0,-1%
echo %dr%

set tmpDir=%temp%\%~n0-Tmp
if not exist "%tmpDir%" md "%tmpDir%"
call copy /Y "%~0" "%tmpDir%"
call copy /Y "%~dpn0.ps1" "%tmpDir%"
call copy /Y "%dr%\%~nx1" "%tmpDir%"

pushd "%tmpDir%"
call "%tmpDir%\%~nx0" "%tmpDir%\%~nx1"
timeout -t 10
@REM pause
goto:eof