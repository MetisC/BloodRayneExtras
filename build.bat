@echo off
setlocal
pushd "%~dp0" || exit /b 1
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
if not defined DevEnvDir call "%VCVARS%" x86 >nul
if errorlevel 1 goto failed
cl /nologo /O2 /MD /W3 /LD br1mod.cpp /link /DEF:dinput8.def /OUT:dinput8.dll /MAP:dinput8.map
if errorlevel 1 goto failed
echo Built dinput8.dll. Close rayne1.exe before deploying and compare hashes.
popd
exit /b 0
:failed
echo Build FAILED. Do not deploy the previous DLL.
popd
exit /b 1
