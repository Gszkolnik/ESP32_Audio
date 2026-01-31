@echo off
setlocal

set IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.5.2
set ADF_PATH=C:\Espressif\frameworks\esp-adf
set IDF_TOOLS_PATH=C:\Espressif
set IDF_PYTHON_ENV_PATH=C:\Espressif\python_env\idf5.5_py3.11_env
set PATH=C:\Espressif\python_env\idf5.5_py3.11_env\Scripts;C:\Espressif\tools\cmake\3.30.2\bin;C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20251107\xtensa-esp-elf\bin;%PATH%
set MSYSTEM=

cd /d C:\Projekty\ESP32_Audio
echo Starting ESP32 build with ESP-ADF...
C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe "%IDF_PATH%\tools\idf.py" build
set BUILD_RESULT=%ERRORLEVEL%
echo Build completed with exit code: %BUILD_RESULT%

endlocal
exit /b %BUILD_RESULT%
