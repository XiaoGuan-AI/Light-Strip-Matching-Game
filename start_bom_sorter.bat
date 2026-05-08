@echo off
setlocal

cd /d "%~dp0"

set "DEFAULT_BASE_URL=http://192.168.3.238:32323"
set "BASE_URL=%~1"
if "%BASE_URL%"=="" set "BASE_URL=%DEFAULT_BASE_URL%"

set "PYTHON_CMD="
where py >nul 2>nul && set "PYTHON_CMD=py -3"
if not defined PYTHON_CMD (
    where python >nul 2>nul && set "PYTHON_CMD=python"
)

if not defined PYTHON_CMD (
    echo [ERROR] 未找到 Python，请先安装 Python 3。
    pause
    exit /b 1
)

echo [INFO] Interactive BOM 前端即将启动
echo [INFO] 分拣箱后端地址: %BASE_URL%
echo [INFO] 正在检查后端连通性...

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ProgressPreference='SilentlyContinue';" ^
  "try {" ^
  "  $resp = Invoke-WebRequest -UseBasicParsing -Uri '%BASE_URL%/api/status' -TimeoutSec 3;" ^
  "  if ($resp.StatusCode -ge 200 -and $resp.StatusCode -lt 500) { exit 0 } else { exit 1 }" ^
  "} catch { exit 1 }"

if errorlevel 1 (
    echo [WARN] 当前无法连接到 %BASE_URL%
    echo [WARN] 仍会继续启动前端页面，你也可以稍后在页面右下角修改地址。
) else (
    echo [OK] 后端接口可访问。
)

cd /d "%~dp0InteractiveBOOM"
call %PYTHON_CMD% open_latest_bom.py --base-url "%BASE_URL%"

endlocal
