@echo off
setlocal EnableExtensions

if "%~2"=="" (
    echo Uso: EXTRACT_CD.bat caminho\jogo.bin CDROOT
    echo      EXTRACT_CD.bat caminho\jogo.iso CDROOT
    echo      EXTRACT_CD.bat caminho\jogo.cue CDROOT
    exit /b 1
)

python tools\extract_psx_cd.py "%~1" "%~2"
if errorlevel 1 exit /b 1

echo.
echo CD extraido em: %~2
echo Agora rode:
echo   psx_recomp_gl.exe caminho\SLPS_027.11 "%~2"
