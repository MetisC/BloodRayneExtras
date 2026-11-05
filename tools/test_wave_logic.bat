@echo off
setlocal
pushd "%~dp0" || exit /b 1
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
if not defined DevEnvDir call "%VCVARS%" x86 >nul
if errorlevel 1 goto failed
if not exist .wave-test mkdir .wave-test
cl /nologo /O2 /MD /W3 /wd4996 test_wave_logic.cpp /Fo:.wave-test\test_wave_logic.obj /Fe:.wave-test\test_wave_logic.exe /link /INCREMENTAL:NO
if errorlevel 1 goto failed
pushd .wave-test
test_wave_logic.exe
set "TEST_RESULT=%ERRORLEVEL%"
popd
popd
exit /b %TEST_RESULT%
:failed
popd
exit /b 1
