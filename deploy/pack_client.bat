@echo off
REM ============================================================
REM Windows 客户端打包脚本
REM 作用：把 Qt 运行库、FFmpeg/SDL DLL 与 config.ini 收集到 dist\vcc-client
REM 用法： pack_client.bat [服务端IP]    默认 127.0.0.1（本机 Docker）
REM 注意： 本文件必须保持 CRLF 换行，否则 cmd.exe 解析会错乱
REM   --with-stunnel [uid]  启用 mTLS 隧道：stunnel 运行时/配置/用户证书一并打进客户端包
REM                         默认关闭；不传时行为与旧版完全一致
REM   --stunnel-dir DIR     stunnel Windows 运行时目录（默认 deploy\stunnel）
REM   --cert-dir DIR        用户 p12 证书目录（默认 deploy\certs）
REM 示例：
REM   pack_client.bat                                     纯明文，等同旧版
REM   pack_client.bat 192.168.1.10                        直连明文 8000
REM   pack_client.bat meet.example.com --with-stunnel u001  走 8443 mTLS
REM ============================================================
setlocal enabledelayedexpansion

REM 仓库根目录 = 本脚本所在 deploy\ 的上一级（%~dp0 已带尾随反斜杠）
set REPO_ROOT=%~dp0..
REM ---- 以下路径按需修改 ----
REM QT_BIN 指向 Qt 的 MinGW 32bit bin 目录
set QT_BIN=D://Qt//5.12.11//mingw73_32//bin
REM BUILD_DIR 指向 Qt Creator 的 shadow build 产物目录（以 .pro 文件名 vcc-client 为准）
set BUILD_DIR=%REPO_ROOT%//build-vcc-client-Desktop_Qt_5_12_11_MinGW_32_bit-release//release
set OUT=%REPO_ROOT%//deploy//dist//vcc-client
set SERVER_IP=127.0.0.1

REM ---- mTLS 隧道开关：默认 0=关闭（行为与旧版一致）----
set WITH_STUNNEL=0
set STUNNEL_DIR=%REPO_ROOT%//deploy//stunnel
set CERT_SRC=%REPO_ROOT%//deploy//certs
set CLIENT_UID=
set GATEWAY_HOST=127.0.0.1
set GATEWAY_PORT=8443
set CONFIG_IP=
REM ---- 参数解析：位置参数=服务器地址；其余为可选开关 ----
:ARGS
if "%~1"=="" goto ARGS_DONE
if /i "%~1"=="--help" goto USAGE
if /i "%~1"=="-h" goto USAGE
if /i "%~1"=="--with-stunnel" goto ARG_STUNNEL
if /i "%~1"=="--stunnel-dir" goto ARG_STUNNEL_DIR
if /i "%~1"=="--cert-dir" goto ARG_CERT_DIR
REM [FIX-P1] %~1 cannot be substring-expanded in batch; copy to a variable first.
REM Reason: the old test never matched, so unknown options fell through and were
REM           treated as SERVER_IP (e.g. "--foo" became the server address).
REM Effect: unknown "--xxx" options are now routed to ARG_UNKNOWN as intended.
set "ARG_CHK=%~1"
if "%ARG_CHK:~0,2%"=="--" goto ARG_UNKNOWN
set SERVER_IP=%~1
shift
goto ARGS

:ARG_STUNNEL
set WITH_STUNNEL=1
shift
if "%~1"=="" goto ARGS
REM [FIX-P1] same substring-expansion fix as above (value-following-option case):
REM           if the next token is another option, do not swallow it as the UID.
set "ARG_CHK=%~1"
if "%ARG_CHK:~0,2%"=="--" goto ARGS
set CLIENT_UID=%~1
shift
goto ARGS

:ARG_STUNNEL_DIR
if "%~2"=="" goto ARG_MISSING
set STUNNEL_DIR=%~2
shift
shift
goto ARGS

:ARG_CERT_DIR
if "%~2"=="" goto ARG_MISSING
set CERT_SRC=%~2
shift
shift
goto ARGS

:ARG_MISSING
echo [ERROR] 参数 %~1 缺少取值
goto USAGE

:ARG_UNKNOWN
echo [ERROR] 未知参数: %~1
goto USAGE

:ARGS_DONE
REM 启用隧道后：客户端连本机 stunnel(127.0.0.1:8000)，stunnel 再去连网关 8443
if "%WITH_STUNNEL%"=="1" goto ARGS_TUNNEL_ON
set CONFIG_IP=%SERVER_IP%
goto ARGS_OK

:ARGS_TUNNEL_ON
set CONFIG_IP=127.0.0.1
set GATEWAY_HOST=%SERVER_IP%

:ARGS_OK

echo === 1/4 清理并创建输出目录 ===
if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%"

echo === 2/4 拷贝主程序与第三方 DLL ===
if not exist "%BUILD_DIR%\vcc-client.exe" (
    echo [ERROR] 找不到 %BUILD_DIR%\vcc-client.exe
    echo         请先用 Qt Creator 以 Release 构建客户端
    exit /b 1
)
copy /Y "%BUILD_DIR%\vcc-client.exe" "%OUT%"
if exist "%BUILD_DIR%\*.dll" copy /Y "%BUILD_DIR%\*.dll" "%OUT%"

echo === 3/4 windeployqt 补齐 Qt 运行库 ===
if not exist "%QT_BIN%\windeployqt.exe" (
    echo [ERROR] 找不到 windeployqt：%QT_BIN%
    exit /b 1
)
"%QT_BIN%\windeployqt.exe" "%OUT%\vcc-client.exe"

echo === 4/4 写入 config.ini（服务端地址 %SERVER_IP%） ===
> "%OUT%\config.ini" echo [Net]
>> "%OUT%\config.ini" echo ip=%CONFIG_IP%

REM ---- 5/5 stunnel 隧道打包：仅 --with-stunnel 生效，默认整段跳过 ----
if not "%WITH_STUNNEL%"=="1" goto PACK_DONE
echo === 5/5 打包 stunnel 隧道（mTLS）===
set TUN=%OUT%\stunnel
if not exist "%TUN%" mkdir "%TUN%"
if not exist "%TUN%\config" mkdir "%TUN%\config"
if not exist "%TUN%\certs" mkdir "%TUN%\certs"
set CONF=%TUN%\config\vcc.conf

if not exist "%STUNNEL_DIR%\stunnel.exe" goto STUNNEL_NORUN
copy /Y "%STUNNEL_DIR%\*.exe" "%TUN%" >nul 2>nul
copy /Y "%STUNNEL_DIR%\*.dll" "%TUN%" >nul 2>nul
echo   stunnel 运行时: %STUNNEL_DIR%
goto STUNNEL_CERT

:STUNNEL_NORUN
echo   [WARN] 运行时目录里没有 stunnel.exe: %STUNNEL_DIR%
echo          请把 stunnel.exe 及其依赖 dll 放进该目录后重跑本脚本
echo          本机安装版通常在 Program Files 下的 stunnel 目录里

:STUNNEL_CERT
if "%CLIENT_UID%"=="" goto STUNNEL_CACERT
if not exist "%CERT_SRC%\%CLIENT_UID%.p12" goto STUNNEL_NOCERT
copy /Y "%CERT_SRC%\%CLIENT_UID%.p12" "%TUN%\certs" >nul
echo   用户证书: %CLIENT_UID%.p12   口令请通过另一渠道告知本人
goto STUNNEL_CACERT

:STUNNEL_NOCERT
echo   [WARN] 找不到该用户的证书包: %CERT_SRC%\%CLIENT_UID%.p12
echo          先签发: bash bare\tls\issue-user.sh --uid %CLIENT_UID% --name 姓名 --email 邮箱

:STUNNEL_CACERT
set CA_LINE=; CAfile = certs\server-ca.pem
if exist "%CERT_SRC%\server-ca.pem" set CA_LINE=CAfile = certs\server-ca.pem
if exist "%CERT_SRC%\server-ca.pem" copy /Y "%CERT_SRC%\server-ca.pem" "%TUN%\certs" >nul

:STUNNEL_CONF
echo   生成隧道配置: %CONF%
> "%CONF%" echo ; 由 pack_client.bat --with-stunnel 自动生成
>> "%CONF%" echo [vcc-mtls]
>> "%CONF%" echo client = yes
>> "%CONF%" echo accept = 127.0.0.1:8000
>> "%CONF%" echo connect = %GATEWAY_HOST%:%GATEWAY_PORT%
if "%CLIENT_UID%"=="" goto STUNNEL_CONF_NOCERT
>> "%CONF%" echo cert = certs\%CLIENT_UID%.p12
goto STUNNEL_CONF_CA

:STUNNEL_CONF_NOCERT
>> "%CONF%" echo ; cert = 请在此填写本人证书路径

:STUNNEL_CONF_CA
>> "%CONF%" echo %CA_LINE%
>> "%CONF%" echo verify = 3
>> "%CONF%" echo sslVersionMin = TLSv1.2
>> "%CONF%" echo TIMEOUTclose = 0
>> "%CONF%" echo TIMEOUTidle = 86400

echo   生成一键启动脚本: %OUT%\start-vcc-client.bat
> "%OUT%\start-vcc-client.bat" echo @echo off
>> "%OUT%\start-vcc-client.bat" echo REM 自动生成：先拉起 stunnel 隧道，再启动 VCC 客户端
>> "%OUT%\start-vcc-client.bat" echo cd /d %%~dp0
>> "%OUT%\start-vcc-client.bat" echo if exist "%%~dp0stunnel\stunnel.exe" start "vcc-tunnel" /min "%%~dp0stunnel\stunnel.exe" "%%~dp0stunnel\config\vcc.conf"
>> "%OUT%\start-vcc-client.bat" echo timeout /t 2 /nobreak ^>nul
>> "%OUT%\start-vcc-client.bat" echo start "" "%%~dp0vcc-client.exe"
echo   隧道模式: config.ini 指向本机 127.0.0.1:8000，由 stunnel 加密上行到 %GATEWAY_HOST%:%GATEWAY_PORT%

:PACK_DONE

echo.
echo === 打包完成： %OUT% ===
echo 服务端地址： %SERVER_IP%
dir "%OUT%" | findstr /i "vcc-client.exe config.ini"
goto END_OK

:USAGE
echo.
echo 用法: pack_client.bat [服务器地址] [选项]
echo.
echo   服务器地址          选填，默认 127.0.0.1（本机 Docker）
echo                       开启隧道时它代表网关主机，config.ini 仍写 127.0.0.1
echo.
echo   选项:
echo     --with-stunnel [uid]  把 stunnel 运行时、隧道配置、用户证书一并打进客户端包
echo                           默认关闭；不传时行为与旧版完全一致
echo     --stunnel-dir DIR     stunnel Windows 运行时目录，默认 deploy\stunnel
echo     --cert-dir DIR        用户 p12 证书目录，默认 deploy\certs
echo     --help               显示本帮助
echo.
echo   示例:
echo     pack_client.bat                                       纯明文打包
echo     pack_client.bat 192.168.1.10                          直连明文 8000
echo     pack_client.bat meet.example.com --with-stunnel u001  走 8443 mTLS
echo.
exit /b 2

:END_OK

endlocal
