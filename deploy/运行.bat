@echo off
cd /d "%~dp0"
if not exist data mkdir data
if not exist output mkdir output
bin\cf_crawler.exe %*
echo.
echo Done! Open output\index.html
pause
