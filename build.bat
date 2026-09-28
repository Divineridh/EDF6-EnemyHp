@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /std:c++17 /EHsc /O2 /MT /LD /W3 ^
   /I "deps\EDF6Plugins" /I "deps\minhook\include" ^
   src\enemyhp.cpp ^
   deps\minhook\src\buffer.c deps\minhook\src\hook.c deps\minhook\src\trampoline.c deps\minhook\src\hde\hde64.c ^
   /Fo:build\ /Fe:build\EDF6EnemyHp.dll ^
   /link user32.lib /IMPLIB:build\EDF6EnemyHp.lib
