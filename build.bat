@echo off
set MSYSTEM=
chcp 65001 >NUL 2>&1
set PYTHONUTF8=1
set PYTHONIOENCODING=utf-8
echo Step 1: calling export.bat > C:\Users\Howar\HowBoyAdvance\build_log.txt 2>&1
call C:\Users\Howar\esp-idf-5.5.1\export.bat >> C:\Users\Howar\HowBoyAdvance\build_log.txt 2>&1
echo Step 2: export done, errorlevel=%ERRORLEVEL% >> C:\Users\Howar\HowBoyAdvance\build_log.txt 2>&1
cd /d C:\Users\Howar\HowBoyAdvance
echo Step 3: running idf.py >> C:\Users\Howar\HowBoyAdvance\build_log.txt 2>&1
idf.py -B build/tanmatsu build -DDEVICE=tanmatsu -DSDKCONFIG_DEFAULTS="sdkconfigs/general;sdkconfigs/tanmatsu" -DSDKCONFIG=sdkconfig_tanmatsu -DIDF_TARGET=esp32p4 -DFAT=0 >> C:\Users\Howar\HowBoyAdvance\build_log.txt 2>&1
echo EXITCODE=%ERRORLEVEL% >> C:\Users\Howar\HowBoyAdvance\build_log.txt 2>&1
