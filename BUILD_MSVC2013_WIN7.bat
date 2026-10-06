@echo off
setlocal EnableExtensions EnableDelayedExpansion

rem ================================================================
rem R3000A Recomp + PSXGPU/OpenGL - Windows 7 / Visual Studio 2013
rem
rem Uso:
rem   BUILD_MSVC2013_WIN7.bat
rem   BUILD_MSVC2013_WIN7.bat C:\jogo\SLPS_027.11
rem   BUILD_MSVC2013_WIN7.bat C:\jogo\SLPS_027.11 C:\jogo\CDROOT
rem
rem Padrao: Win32/x86. Para x64, antes de chamar:
rem   set PSX_BUILD_ARCH=x64
rem ================================================================

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "THIRD=%ROOT%\third_party"
set "GLFW_VERSION=3.3.8"
set "GLFW_DIR=%THIRD%\glfw"
set "GLFW_ZIP=%THIRD%\glfw-%GLFW_VERSION%.zip"
set "GLFW_URL=https://github.com/glfw/glfw/archive/refs/tags/%GLFW_VERSION%.zip"

if /I "%PSX_BUILD_ARCH%"=="x64" (
    set "VCVARS_ARCH=amd64"
    set "VS_GENERATOR=Visual Studio 12 2013 Win64"
    set "BUILD_DIR=%ROOT%\build_vs2013_x64"
) else (
    set "VCVARS_ARCH=x86"
    set "VS_GENERATOR=Visual Studio 12 2013"
    set "BUILD_DIR=%ROOT%\build_vs2013_x86"
)

pushd "%ROOT%" >nul

call :find_cmake
if errorlevel 1 goto :fail

call :find_vs2013
if errorlevel 1 goto :fail

call "%VCVARS%" %VCVARS_ARCH% >nul
if errorlevel 1 (
    echo [ERRO] vcvarsall.bat falhou.
    goto :fail
)

echo [OK] Visual Studio 2013: %VCVARS%
echo [OK] Arquitetura: %VCVARS_ARCH%
echo [OK] CMake: %CMAKE_EXE%

call :ensure_glfw
if errorlevel 1 goto :fail

echo.
echo [1/4] Configurando projeto...
"%CMAKE_EXE%" -S "%ROOT%" -B "%BUILD_DIR%" -G "%VS_GENERATOR%" ^
    -DPSXRECOMP_BUILD_GL=ON ^
    -DPSXRECOMP_BUILD_GLFW_APP=ON ^
    -DPSXRECOMP_BUILD_TESTS=OFF ^
    -DGLFW_BUILD_EXAMPLES=OFF ^
    -DGLFW_BUILD_TESTS=OFF ^
    -DGLFW_BUILD_DOCS=OFF ^
    -DGLFW_INSTALL=OFF
if errorlevel 1 goto :cmake_fail

echo.
echo [2/4] Compilando o recompilador R3000A...
"%CMAKE_EXE%" --build "%BUILD_DIR%" --config Release --target r3000a_recomp
if errorlevel 1 goto :build_fail

if not "%~1"=="" (
    if not exist "%~1" (
        echo [ERRO] PS-X EXE nao encontrado: "%~1"
        goto :fail
    )

    echo.
    echo [3/4] Gerando C++ dividido a partir de:
    echo       "%~1"

    if not exist "%ROOT%\generated" mkdir "%ROOT%\generated"
    del /q "%ROOT%\generated\game_dispatch.cpp" 2>nul
    del /q "%ROOT%\generated\game_part_*.cpp" 2>nul
    del /q "%ROOT%\generated\game_helpers.h" 2>nul
    del /q "%ROOT%\generated\game_parts.h" 2>nul

    "%BUILD_DIR%\Release\r3000a_recomp.exe" "%~1" --split "%ROOT%\generated" 4096
    if errorlevel 1 goto :recomp_fail

    rem Reconfigure para o CMake enxergar game_dispatch.cpp/game_part_*.cpp.
    "%CMAKE_EXE%" -S "%ROOT%" -B "%BUILD_DIR%" -G "%VS_GENERATOR%" ^
        -DPSXRECOMP_BUILD_GL=ON ^
        -DPSXRECOMP_BUILD_GLFW_APP=ON ^
        -DPSXRECOMP_BUILD_TESTS=OFF ^
        -DGLFW_BUILD_EXAMPLES=OFF ^
        -DGLFW_BUILD_TESTS=OFF ^
        -DGLFW_BUILD_DOCS=OFF ^
        -DGLFW_INSTALL=OFF
    if errorlevel 1 goto :cmake_fail
) else (
    echo.
    echo [3/4] Nenhum PS-X EXE informado. Compilando com game_stub.cpp.
)

echo.
echo [4/4] Compilando runner PSX + GLFW + OpenGL 3.3...
"%CMAKE_EXE%" --build "%BUILD_DIR%" --config Release --target psx_recomp_gl
if errorlevel 1 goto :build_fail

echo.
echo ================================================================
echo BUILD OK

echo Recompilador:
echo   "%BUILD_DIR%\Release\r3000a_recomp.exe"
echo Runner:
echo   "%BUILD_DIR%\Release\psx_recomp_gl.exe"

if not "%~1"=="" (
    if not "%~2"=="" (
        echo.
        echo Para executar:
        echo   "%BUILD_DIR%\Release\psx_recomp_gl.exe" "%~1" "%~2"
    ) else (
        echo.
        echo Para executar sem CD montado:
        echo   "%BUILD_DIR%\Release\psx_recomp_gl.exe" "%~1"
        echo.
        echo Ou com BIN/ISO/CDROOT:
        echo   "%BUILD_DIR%\Release\psx_recomp_gl.exe" "%~1" "C:\caminho\CDROOT"
    )
)

echo ================================================================
popd >nul
exit /b 0

:find_cmake
set "CMAKE_EXE="
for /f "delims=" %%I in ('where cmake.exe 2^>nul') do if not defined CMAKE_EXE set "CMAKE_EXE=%%I"
if not defined CMAKE_EXE (
    if exist "C:\Program Files\CMake\bin\cmake.exe" set "CMAKE_EXE=C:\Program Files\CMake\bin\cmake.exe"
)
if not defined CMAKE_EXE (
    if exist "C:\Program Files (x86)\CMake\bin\cmake.exe" set "CMAKE_EXE=C:\Program Files (x86)\CMake\bin\cmake.exe"
)
if not defined CMAKE_EXE (
    echo [ERRO] cmake.exe nao encontrado. Instale CMake 3.16+ e coloque no PATH.
    exit /b 1
)
exit /b 0

:find_vs2013
set "VCVARS="

if defined VS120COMNTOOLS (
    if exist "%VS120COMNTOOLS%..\..\VC\vcvarsall.bat" set "VCVARS=%VS120COMNTOOLS%..\..\VC\vcvarsall.bat"
)

if not defined VCVARS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio 12.0\VC\vcvarsall.bat" set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio 12.0\VC\vcvarsall.bat"
if not defined VCVARS if exist "%ProgramFiles%\Microsoft Visual Studio 12.0\VC\vcvarsall.bat" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio 12.0\VC\vcvarsall.bat"

for %%D in (C D E F G H) do (
    if not defined VCVARS if exist "%%D:\Program Files (x86)\Microsoft Visual Studio 12.0\VC\vcvarsall.bat" set "VCVARS=%%D:\Program Files (x86)\Microsoft Visual Studio 12.0\VC\vcvarsall.bat"
    if not defined VCVARS if exist "%%D:\Arquivos de Programas (x86)\Microsoft Visual Studio 12.0\VC\vcvarsall.bat" set "VCVARS=%%D:\Arquivos de Programas (x86)\Microsoft Visual Studio 12.0\VC\vcvarsall.bat"
    if not defined VCVARS if exist "%%D:\Microsoft Visual Studio 12.0\VC\vcvarsall.bat" set "VCVARS=%%D:\Microsoft Visual Studio 12.0\VC\vcvarsall.bat"
)

if not defined VCVARS (
    echo [ERRO] Visual Studio 2013 / VC12 nao encontrado.
    echo        Precisa existir VC\vcvarsall.bat.
    exit /b 1
)
exit /b 0

:ensure_glfw
if exist "%GLFW_DIR%\CMakeLists.txt" (
    echo [OK] GLFW ja existe em third_party\glfw
    exit /b 0
)

if not exist "%THIRD%" mkdir "%THIRD%"
if exist "%GLFW_DIR%" rmdir /s /q "%GLFW_DIR%"
if exist "%THIRD%\glfw-%GLFW_VERSION%" rmdir /s /q "%THIRD%\glfw-%GLFW_VERSION%"

echo.
echo [GLFW] Baixando GLFW %GLFW_VERSION%...
if exist "%GLFW_ZIP%" del /q "%GLFW_ZIP%"

rem PowerShell 2.0/Win7: WebClient + TLS 1.2 numerico.
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -Command ^
 "$ErrorActionPreference='Stop'; try {[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]3072} catch {}; $w=New-Object System.Net.WebClient; $w.DownloadFile('%GLFW_URL%','%GLFW_ZIP%')" >nul 2>nul

if not exist "%GLFW_ZIP%" (
    echo [GLFW] PowerShell falhou. Tentando certutil...
    certutil.exe -urlcache -split -f "%GLFW_URL%" "%GLFW_ZIP%" >nul 2>nul
)

if exist "%GLFW_ZIP%" (
    echo [GLFW] Extraindo...
    where 7z.exe >nul 2>nul
    if not errorlevel 1 (
        7z.exe x -y "%GLFW_ZIP%" -o"%THIRD%" >nul
    ) else (
        powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -Command ^
         "$ErrorActionPreference='Stop'; $s=New-Object -ComObject Shell.Application; $z=$s.NameSpace('%GLFW_ZIP%'); $d=$s.NameSpace('%THIRD%'); if($z -eq $null -or $d -eq $null){exit 2}; $d.CopyHere($z.Items(),20)" >nul 2>nul

        rem CopyHere pode ser assincrono no Windows 7. Espera pelo CMakeLists.
        for /L %%N in (1,1,30) do (
            if exist "%THIRD%\glfw-%GLFW_VERSION%\CMakeLists.txt" goto :glfw_extracted
            ping 127.0.0.1 -n 2 >nul
        )
    )
)

:glfw_extracted
if exist "%THIRD%\glfw-%GLFW_VERSION%\CMakeLists.txt" (
    move /Y "%THIRD%\glfw-%GLFW_VERSION%" "%GLFW_DIR%" >nul
)

if not exist "%GLFW_DIR%\CMakeLists.txt" (
    echo [GLFW] ZIP falhou. Tentando git clone...
    where git.exe >nul 2>nul
    if not errorlevel 1 (
        if exist "%GLFW_DIR%" rmdir /s /q "%GLFW_DIR%"
        git clone --depth 1 --branch %GLFW_VERSION% https://github.com/glfw/glfw.git "%GLFW_DIR%"
    )
)

if not exist "%GLFW_DIR%\CMakeLists.txt" (
    echo [ERRO] Nao foi possivel baixar/extrair GLFW %GLFW_VERSION%.
    echo        Em Win7 antigo, confira TLS 1.2.
    echo        Alternativa: extraia manualmente GLFW %GLFW_VERSION% em:
    echo        "%GLFW_DIR%"
    exit /b 1
)

echo [OK] GLFW %GLFW_VERSION% pronto.
exit /b 0

:cmake_fail
echo.
echo [ERRO] Falha ao configurar com CMake.
echo        Confira Visual Studio 2013 e Windows SDK 8.0/8.1.
goto :fail

:recomp_fail
echo.
echo [ERRO] Falha ao gerar o C++ recompilado.
goto :fail

:build_fail
echo.
echo [ERRO] Falha de compilacao MSVC2013.
echo        Veja as primeiras mensagens C/C++ acima.
goto :fail

:fail
popd >nul
exit /b 1
