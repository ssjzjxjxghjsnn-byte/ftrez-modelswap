@echo off
setlocal
pushd "%~dp0"
if "%~1"=="" (
    call gradlew.bat :app:assembleDebug :app:lintDebug
) else (
    call gradlew.bat %*
)
set "BUILD_EXIT_CODE=%ERRORLEVEL%"
popd
exit /b %BUILD_EXIT_CODE%
