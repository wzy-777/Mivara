@echo off
cd /d "%~dp0"
rem 用法: 启动投屏发送.bat <接收端IP>
rem 示例: 启动投屏发送.bat 10.190.85.16
if "%~1"=="" (
    echo 用法: %~nx0 ^<接收端IP^>
    exit /b 1
)
start "" "%~dp0..\bin\sender.exe" %~1 8081
