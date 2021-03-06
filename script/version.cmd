@ECHO OFF
SET FILENAME=%1
IF "%2"=="" (SET SEPARATOR=.) ELSE (SET SEPARATOR=%2)
IF "%SEPARATOR%"=="-comma" SET SEPARATOR=,
IF "%3"=="-extra" (SET EXTRA_TEXT=%SEPARATOR%0) ELSE (SET EXTRA_TEXT=)

SETLOCAL ENABLEEXTENSIONS
FOR /F "tokens=3" %%a IN (
'FINDSTR /C:"#define VERSION_MAJOR" %FILENAME%'
) DO (
SET VERSION_MAJOR=%%~a
) 
FOR /F "tokens=3" %%a IN (
'FINDSTR /C:"#define VERSION_MINOR" %FILENAME%'
) DO (
SET VERSION_MINOR=%%~a
) 
FOR /F "tokens=3" %%a IN (
'FINDSTR /C:"#define VERSION_RELEASE" %FILENAME%'
) DO (
SET VERSION_RELEASE=%%~a
) 
FOR /F "tokens=3" %%a IN (
'FINDSTR /C:"#define VERSION_PATCH" %FILENAME%'
) DO (
SET VERSION_PATCH=%%~a
) 
IF "%3"=="-patch" (SET EXTRA_TEXT=%VERSION_PATCH%)

ECHO %VERSION_MAJOR%%SEPARATOR%%VERSION_MINOR%%SEPARATOR%%VERSION_RELEASE%%EXTRA_TEXT%