@echo off
setlocal

cd /d "%~dp0"

echo ============================================================
echo R3000A PSX - BUILD SLPS_027.11
echo WinLibs MinGW32
echo ============================================================
echo.

REM ============================================================
REM WINLIBS
REM IMPORTANTE:
REM Para argumentos enviados ao CMake usamos /
REM ============================================================

set "MINGW=F:\mingw64\mingw32"

set "CC_WIN=F:\mingw64\mingw32\bin\gcc.exe"
set "CXX_WIN=F:\mingw64\mingw32\bin\g++.exe"
set "MAKE_WIN=F:\mingw64\mingw32\bin\mingw32-make.exe"

set "CC=F:/mingw64/mingw32/bin/gcc.exe"
set "CXX=F:/mingw64/mingw32/bin/g++.exe"
set "MINGW_MAKE=F:/mingw64/mingw32/bin/mingw32-make.exe"


echo [1/6] Verificando WinLibs...

if not exist "%CC_WIN%" (
	echo.
	echo ERRO: gcc.exe nao encontrado:
	echo %CC_WIN%
	pause
	exit /b 1
)

if not exist "%CXX_WIN%" (
	echo.
	echo ERRO: g++.exe nao encontrado:
	echo %CXX_WIN%
	pause
	exit /b 1
)

if not exist "%MAKE_WIN%" (
	echo.
	echo ERRO: mingw32-make.exe nao encontrado:
	echo %MAKE_WIN%
	pause
	exit /b 1
)


REM ============================================================
REM PATH
REM ============================================================

set "PATH=%MINGW%\bin;%PATH%"

echo.
echo GCC:
"%CC_WIN%" --version

echo.
echo G++:
"%CXX_WIN%" --version


REM ============================================================
REM REMOVE COMPLETAMENTE CACHE ANTIGO
REM ============================================================

echo.
echo [2/6] Limpando build antigo...

if exist "build_mingw32" (
	rmdir /s /q "build_mingw32"
)

if exist "CMakeCache.txt" (
	del /q "CMakeCache.txt"
)

if exist "CMakeFiles" (
	rmdir /s /q "CMakeFiles"
)


REM ============================================================
REM CONFIGURA
REM ============================================================

echo.
echo [3/6] Configurando CMake com MinGW Makefiles...

cmake ^
	-S . ^
	-B build_mingw32 ^
	-G "MinGW Makefiles" ^
	-DCMAKE_MAKE_PROGRAM=%MINGW_MAKE% ^
	-DCMAKE_C_COMPILER=%CC% ^
	-DCMAKE_CXX_COMPILER=%CXX% ^
	-DCMAKE_BUILD_TYPE=Debug ^
	-DPSXRECOMP_BUILD_GL=ON ^
	-DPSXRECOMP_BUILD_GLFW_APP=ON ^
	-DPSXRECOMP_BUILD_TESTS=OFF

if errorlevel 1 (
	echo.
	echo ============================================================
	echo ERRO CONFIGURANDO CMAKE
	echo ============================================================
	echo.
	pause
	exit /b 1
)


REM ============================================================
REM COMPILA RECOMPILADOR R3000A
REM ============================================================

echo.
echo [4/6] Compilando r3000a_recomp...

cmake ^
	--build build_mingw32 ^
	--target r3000a_recomp ^
	--parallel 4

if errorlevel 1 (
	echo.
	echo ============================================================
	echo ERRO COMPILANDO R3000A_RECOMP
	echo ============================================================
	echo.
	pause
	exit /b 1
)


REM ============================================================
REM LOCALIZA RECOMPILADOR
REM ============================================================

set "RECOMP=build_mingw32\r3000a_recomp.exe"

if not exist "%RECOMP%" (

	if exist "build_mingw32\Release\r3000a_recomp.exe" (
		set "RECOMP=build_mingw32\Release\r3000a_recomp.exe"
	)

)

if not exist "%RECOMP%" (
	echo.
	echo ERRO:
	echo r3000a_recomp.exe nao foi encontrado.
	echo.
	echo Procurando...
	dir /s /b "build_mingw32\r3000a_recomp.exe"
	echo.
	pause
	exit /b 1
)


REM ============================================================
REM SLPS
REM ============================================================

echo.
echo [5/6] Verificando SLPS_027.11...

if not exist "SLPS_027.11" (
	echo.
	echo ERRO:
	echo SLPS_027.11 nao encontrado.
	echo.
	echo Coloque:
	echo.
	echo     SLPS_027.11
	echo.
	echo na raiz:
	echo.
	echo     %CD%
	echo.
	pause
	exit /b 1
)


REM ============================================================
REM LIMPA CODIGO GERADO ANTIGO
REM ============================================================

if not exist "generated" (
	mkdir "generated"
)

del /q "generated\game_dispatch.cpp" 2>nul
del /q "generated\game_part_*.cpp" 2>nul
del /q "generated\generated_game.cpp" 2>nul


REM ============================================================
REM RECOMPILA O EXECUTAVEL PS1
REM ============================================================

echo.
echo Recompilando SLPS_027.11...
echo.
echo Executavel:
echo %RECOMP%
echo.

"%RECOMP%" "SLPS_027.11" "generated"

if errorlevel 1 (
	echo.
	echo ============================================================
	echo ERRO RECOMPILANDO SLPS_027.11
	echo ============================================================
	echo.
	pause
	exit /b 1
)


REM ============================================================
REM MOSTRA ARQUIVOS GERADOS
REM ============================================================

echo.
echo Codigo gerado:
echo.

dir /b "generated\*.cpp"

echo.


REM ============================================================
REM RECONFIGURA
REM
REM Isso e necessario porque os game_part_*.cpp acabaram
REM de ser criados.
REM ============================================================

echo [6/6] Reconfigurando projeto com game gerado...

cmake ^
	-S . ^
	-B build_mingw32 ^
	-G "MinGW Makefiles" ^
	-DCMAKE_MAKE_PROGRAM=%MINGW_MAKE% ^
	-DCMAKE_C_COMPILER=%CC% ^
	-DCMAKE_CXX_COMPILER=%CXX% ^
	-DCMAKE_BUILD_TYPE=Debug ^
	-DPSXRECOMP_BUILD_GL=ON ^
	-DPSXRECOMP_BUILD_GLFW_APP=ON ^
	-DPSXRECOMP_BUILD_TESTS=OFF

if errorlevel 1 (
	echo.
	echo ============================================================
	echo ERRO RECONFIGURANDO CMAKE
	echo ============================================================
	echo.
	pause
	exit /b 1
)


REM ============================================================
REM COMPILA GAME + GLFW + OPENGL
REM ============================================================

echo.
echo Compilando psx_recomp_gl...
echo.

cmake ^
	--build build_mingw32 ^
	--target psx_recomp_gl ^
	--parallel 4

if errorlevel 1 (
	echo.
	echo ============================================================
	echo ERRO COMPILANDO PSX_RECOMP_GL
	echo ============================================================
	echo.
	pause
	exit /b 1
)


REM ============================================================
REM RESULTADO
REM ============================================================

echo.
echo ============================================================
echo BUILD CONCLUIDO
echo ============================================================
echo.

if exist "build_mingw32\psx_recomp_gl.exe" (
	echo Executavel:
	echo.
	echo     build_mingw32\psx_recomp_gl.exe
	echo.
) else (
	echo Procurando psx_recomp_gl.exe...
	echo.
	dir /s /b "build_mingw32\psx_recomp_gl.exe"
)

echo.
echo Para executar:
echo.
echo     build_mingw32\psx_recomp_gl.exe
echo.

pause
endlocal