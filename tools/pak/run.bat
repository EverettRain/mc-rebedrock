@echo off
rem mc-rebedrock (pre-built binary) — Windows launcher.
rem Just runs the bundled game; there is no build step.
cd /d "%~dp0"
bin\mc_rebedrock.exe %*
