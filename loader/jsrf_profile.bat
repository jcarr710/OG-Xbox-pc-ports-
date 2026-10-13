@echo off
rem Runs JSRF PC with performance logging, shown in this window and written
rem to jsrf.log: frame rate and GPU-thread load every 2 s (--perf), and where
rem each game thread spends its time every 10 s (--profile).
cd /d "%~dp0"
jsrf.exe --profile --perf %*
