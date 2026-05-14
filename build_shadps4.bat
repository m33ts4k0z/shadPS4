@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > nul
cmake --build Build/x64-Clang-Release --target shadps4 -j 12
