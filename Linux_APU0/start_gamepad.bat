@echo off
chcp 65001 >nul
title EG400 Gamepad Bridge
set BOARD_IP=192.168.137.100

echo ============================================
echo  EG400 Gamepad Bridge
echo ============================================
echo  Board IP (edit this file to change): %BOARD_IP%
echo.
echo  [1] Xbox  debug : print indices only, no network
echo  [2] Xbox  connect
echo  [3] Switch/ShanWan connect (uses controller_map_shanwan.json)
echo  [4] Switch Pro learn     (print indices, build your own map)
echo  [5] connect with manual IP
echo ============================================
set /p MODE=Select mode (1/2/3/4/5):

cd /d "%~dp0reference\legacy_phytium"

if "%MODE%"=="1" goto :debug
if "%MODE%"=="4" goto :learn
if "%MODE%"=="5" goto :askip
if "%MODE%"=="3" goto :swmode
if "%MODE%"=="2" goto :xmode
echo Unknown option.
goto :done

:askip
set /p BOARD_IP=Enter board IP: 
goto :xmode

:xmode
call :checknet
python controller_bridge.py --host %BOARD_IP%
goto :done

:swmode
call :checknet
python controller_bridge.py --host %BOARD_IP% --map controller_map_shanwan.json
goto :done

:debug
python controller_bridge.py --debug
goto :done

:learn
python controller_bridge.py --learn
goto :done

:checknet
rem 只提示、不拦截：手柄走 UDP，ping 不通也可能是 ICMP 被拦或首次 ARP 未就绪
ping -n 2 -w 2000 %BOARD_IP% >nul
if errorlevel 1 (
    echo.
    echo [WARN] ping %BOARD_IP% 没有回应，仍继续启动桥接（UDP 不依赖 ICMP）。
    echo        如果板上没反应，请检查：
    echo          - PC 网卡 IP 是否在 192.168.137.x（静态 192.168.137.254）
    echo          - 板卡是否已 start_all 完成、网线插好
    echo          - 防火墙是否拦截出站 UDP 9002
    echo.
)
exit /b 0

:done
echo.
echo Bridge exited.
pause
