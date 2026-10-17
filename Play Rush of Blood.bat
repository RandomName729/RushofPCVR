@echo off
rem Until Dawn: Rush of Blood on this PC, shown in a headset connected through Virtual Desktop
rem (or any other OpenXR runtime). Settings: pc-vr\settings.txt
title RushVR
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0pc-vr\launch.ps1" %*
