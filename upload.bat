@echo off
set MSYSTEM=
cd /d C:\Users\Howar\HowBoyAdvance\badgelink\tools
call .venv\Scripts\activate.bat
python badgelink.py appfs upload howboyadvance "HowBoyAdvance" 0 ..\..\build\tanmatsu\application.bin
