@echo off
rem FEX's generators call "python3", which Windows does not have. This runs the Python
rem named in tools\python.local (a full path, one line), or "python" from PATH.
setlocal
set "py=python"
if exist "%~dp0..\python.local" set /p py=<"%~dp0..\python.local"
"%py%" %*
