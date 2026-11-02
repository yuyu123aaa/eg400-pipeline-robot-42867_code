@echo off
chcp 65001 >nul
title EG400 板卡一键启动

echo ============================================
echo  EG400 板卡一键启动（对时 + 启动服务）
echo ============================================
echo  目标: 192.168.137.100
echo.

REM [1/3] 等待板卡SSH就绪
echo [1/3] 等待板卡SSH就绪...
:waitssh
ping -n 1 192.168.137.100 >nul 2>&1
if errorlevel 1 (
    echo   板卡未响应，5秒后重试...
    timeout /t 5 /nobreak >nul
    goto waitssh
)
echo   板卡已就绪

REM [2/3] 从电脑取当前时间同步到板卡
echo [2/3] 同步电脑时间到板卡...
for /f "delims=" %%i in ('powershell -Command "Get-Date -Format 'yyyy-MM-dd HH:mm:ss'"') do set NOW=%%i
echo   电脑时间: %NOW%
ssh -t uisrc@192.168.137.100 "sudo date -s '%NOW%' && date"

REM [3/3] 启动服务
echo.
echo [3/3] 启动服务...
ssh -t uisrc@192.168.137.100 "sudo sh ~/eg400_start_all.sh"

echo.
echo ============================================
echo  启动完成
echo  浏览器: http://192.168.137.100:5000/
echo  手柄:   运行 start_gamepad.bat
echo ============================================
pause
