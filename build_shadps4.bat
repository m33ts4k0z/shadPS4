@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > nul
if not exist Build\x64-Clang-Release\build.ninja (
    cmake --preset x64-Clang-Release
    if errorlevel 1 exit /b 1
)
cmake --build Build/x64-Clang-Release --target shadps4 -j 12
