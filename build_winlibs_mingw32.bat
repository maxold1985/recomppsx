@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

set "CXX=F:\mingw64\mingw32\bin\g++.exe"
set "CC=F:\mingw64\mingw32\bin\gcc.exe"
set "AR=F:\mingw64\mingw32\bin\ar.exe"

if not exist "%CXX%" (
 echo [ERRO] g++.exe nao encontrado: %CXX%
 exit /b 1
)
if not exist "%CC%" (
 echo [ERRO] gcc.exe nao encontrado: %CC%
 exit /b 1
)

set "GLFW=glfw"
if not exist "%GLFW%\src\init.c" set "GLFW=third_party\glfw"
if not exist "%GLFW%\src\init.c" (
 echo [ERRO] Codigo-fonte do GLFW3 nao encontrado.
 echo.
 echo Coloque o GLFW em uma destas pastas:
 echo   %CD%\glfw
 echo   %CD%\third_party\glfw
 echo.
 echo Deve existir, por exemplo: glfw\src\init.c e glfw\include\GLFW\glfw3.h
 exit /b 2
)

set "OBJ=build_winlibs\obj"
set "BIN=build_winlibs\bin"
set "GLOBJ=%OBJ%\glfw"
if not exist "%OBJ%" mkdir "%OBJ%"
if not exist "%GLOBJ%" mkdir "%GLOBJ%"
if not exist "%BIN%" mkdir "%BIN%"

set "CFLAGS=-O2 -D_GLFW_WIN32 -DWIN32_LEAN_AND_MEAN -I%GLFW%\include -I%GLFW%\src"
set "CXXFLAGS=-std=gnu++11 -O2 -DWIN32_LEAN_AND_MEAN -Iinclude -Ithird_party\psxgpu\include -Ithird_party\psxgpu\src -I%GLFW%\include"

rem GLFW 3 Win32 + WGL compilado do codigo-fonte.
set GLFW_SRC=init.c context.c input.c monitor.c platform.c vulkan.c window.c win32_init.c win32_joystick.c win32_module.c win32_monitor.c win32_thread.c win32_time.c win32_window.c wgl_context.c egl_context.c osmesa_context.c

rem Algumas versoes antigas do GLFW nao possuem platform.c/win32_module.c/vulkan.c.
set GLFW_OBJS=
for %%F in (%GLFW_SRC%) do (
 if exist "%GLFW%\src\%%F" (
   echo [CC GLFW] %%F
   "%CC%" %CFLAGS% -c "%GLFW%\src\%%F" -o "%GLOBJ%\%%~nF.o"
   if errorlevel 1 exit /b 10
   set "GLFW_OBJS=!GLFW_OBJS! %GLOBJ%\%%~nF.o"
 )
)

if exist "%AR%" (
 echo [AR] libglfw3.a
 "%AR%" rcs "%OBJ%\libglfw3.a" !GLFW_OBJS!
 if errorlevel 1 exit /b 11
 set "GLFW_LINK=%OBJ%\libglfw3.a"
) else (
 set "GLFW_LINK=!GLFW_OBJS!"
)

set CPP_SRC=src\decoder.cpp src\psxexe.cpp src\recompiler.cpp src\irq.cpp src\timers.cpp src\cdrom.cpp src\spu.cpp src\mdec.cpp src\pad_sio.cpp src\gte.cpp src\bios_hle.cpp src\psx_memory.cpp src\psx_runtime.cpp third_party\psxgpu\src\psx_gpu.cpp third_party\psxgpu\src\psx_gpu_mmio.cpp third_party\psxgpu\src\gl_api.cpp third_party\psxgpu\src\psx_gpu_gl.cpp app\main_glfw.cpp
set CPP_OBJS=
for %%F in (%CPP_SRC%) do (
 set "N=%%~nF"
 echo [CXX] %%F
 "%CXX%" %CXXFLAGS% -c "%%F" -o "%OBJ%\!N!.o"
 if errorlevel 1 exit /b 20
 set "CPP_OBJS=!CPP_OBJS! %OBJ%\!N!.o"
)

rem Usa game.cpp se houver codigo recompilado; senao usa stub para permitir testar o runtime.
set "GAME_SRC=generated\game.cpp"
if not exist "%GAME_SRC%" set "GAME_SRC=generated\game_stub.cpp"
echo [CXX] %GAME_SRC%
"%CXX%" %CXXFLAGS% -c "%GAME_SRC%" -o "%OBJ%\generated_game.o"
if errorlevel 1 exit /b 21

rem Se o recompilador tiver emitido partes separadas, compile-as tambem.
set PART_OBJS=
for %%F in (generated\game_part_*.cpp) do (
 if exist "%%F" (
   echo [CXX] %%F
   "%CXX%" %CXXFLAGS% -c "%%F" -o "%OBJ%\%%~nF.o"
   if errorlevel 1 exit /b 22
   set "PART_OBJS=!PART_OBJS! %OBJ%\%%~nF.o"
 )
)

rem game_dispatch.cpp substitui game.cpp quando existe saida split.
if exist "generated\game_dispatch.cpp" (
 echo [CXX] generated\game_dispatch.cpp
 "%CXX%" %CXXFLAGS% -c "generated\game_dispatch.cpp" -o "%OBJ%\game_dispatch.o"
 if errorlevel 1 exit /b 23
 set "GAMEOBJ=%OBJ%\game_dispatch.o !PART_OBJS!"
) else (
 set "GAMEOBJ=%OBJ%\generated_game.o"
)

echo [LINK] recompgl.exe
"%CXX%" -o "%BIN%\recompgl.exe" !CPP_OBJS! !GAMEOBJ! !GLFW_LINK! -lopengl32 -lgdi32 -luser32 -lshell32 -lwinmm -lole32 -luuid
if errorlevel 1 exit /b 30

echo.
echo ===============================================
echo BUILD OK: %CD%\%BIN%\recompgl.exe
echo GLFW3 foi compilado do codigo-fonte neste build.
echo ===============================================
exit /b 0
