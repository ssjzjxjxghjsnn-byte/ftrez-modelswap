@echo off
setlocal
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "FRAMEHOOK_VS=%%i"
if not defined FRAMEHOOK_VS exit /b 2
call "%FRAMEHOOK_VS%\Common7\Tools\VsDevCmd.bat" -no_logo -arch=amd64 -host_arch=amd64
if errorlevel 1 exit /b 2
cd /d "%~dp0..\.."
if not exist build\native-tests mkdir build\native-tests
rem Existing UnityApi uses size_t for a metadata parameter index (C4267).
cl /nologo /std:c++20 /EHsc /W4 /WX /wd4267 /Itests\native\stubs /Iapp\src\main\cpp /Iapp\src\main\cpp\third_party\Dobby\include tests\native\frame_hook_test.cpp app\src\main\cpp\il2cpp\frame_hook.cpp /Fo"build\native-tests\\" /Fe"build\native-tests\frame_hook_test.exe"
if errorlevel 1 exit /b 1
for %%s in (success first_error_null first_commit_prewrite_error first_commit_postwrite_error second_error_null second_commit_prewrite_error second_commit_postwrite_error) do (
    build\native-tests\frame_hook_test.exe %%s
    if errorlevel 1 exit /b 1
)
exit /b 0
