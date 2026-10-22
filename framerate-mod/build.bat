@echo off
setlocal

set "VCVARSALL="
for %%v in (
    "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
) do (
    if exist %%v (
        set "VCVARSALL=%%~v"
        goto :found
    )
)
echo error: could not find vcvarsall.bat
exit /b 1

:found
echo Using: %VCVARSALL%

call "%VCVARSALL%" x86 >nul 2>&1

if not exist "build" mkdir build

echo compiling...
cl.exe /nologo /O2 /MT /W3 /EHsc ^
    /I src ^
    /D "WIN32" /D "NDEBUG" /D "_WINDOWS" /D "_USRDLL" /D "_CRT_SECURE_NO_WARNINGS" ^
    src\main.cpp ^
    /link /DLL /DEF:src\dinput8.def /OUT:build\dinput8.dll ^
    kernel32.lib user32.lib psapi.lib winmm.lib

if errorlevel 1 (
    echo build faled
    exit /b 1
)

copy /Y framerate-mod.ini build\framerate-mod.ini >nul

echo.
echo build successful: build\dinput8.dll + build\framerate-mod.ini
echo.

