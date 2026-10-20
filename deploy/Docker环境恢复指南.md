# Docker 环境恢复指南（放行 wsl.exe → 拉起 Docker）

适用症状：

```
failed to connect to the docker API at npipe:////./pipe/dockerDesktopLinuxEngine;
check if the path is correct and if the daemon is running:
open //./pipe/dockerDesktopLinuxEngine: The system cannot find the file specified.
```

或：

```
PROGRAM BLOCKED BY SECURITY POLICY - The sandbox prevented a program on the
configured Program Blacklist from starting: wsl.exe
```

**根因**：Docker Desktop 用的是 WSL2 后端，它必须调用 `C:\Windows\system32\wsl.exe` 才能把 Linux 引擎拉起来。
这个可执行文件被安全策略拦下 → 引擎永远起不来 → 命名管道 `dockerDesktopLinuxEngine` 不存在 → 所有 `docker` 命令失败。
**Docker Desktop 本身没坏，换 Docker 版本没用。**

---

## 路线 A（推荐）：放行 `wsl.exe`

### 步骤 1 — 在 WorkBuddy 里放行

拦截来自 WorkBuddy 自身的命令黑名单，所以要在它里面放行：

1. 打开 WorkBuddy 客户端
2. 进入 **安全中心**（Setting / 安全中心）
3. 选择 **命令安全**（Command Security）
4. 找到 **程序黑名单**（Program Blacklist）
5. 在列表里定位 `C:\Windows\system32\wsl.exe`，点**移除 / 允许**
6. 完全退出 WorkBuddy 再重开（让策略生效）

> 如果你是在别的 AI 工具 / 沙箱里触发的拦截，就去那个工具里放行，原理一样。

### 步骤 2 — 检查第三方安全软件

火绒 / 360 / Windows Defender 的应用控制也可能拦 `wsl.exe`，一并加白名单：

- **Windows 安全中心** → 病毒和威胁防护 → 管理勒索软件防护 → "受控文件夹访问"（若开启，加允许应用 `wsl.exe`）
- **火绒** → 防护中心 → 系统加固 / 启动项管理 → 允许 `wsl.exe`
- **360** → 安全防护中心 → 信任与阻止 → 添加信任 `C:\Windows\system32\wsl.exe`

### 步骤 3 — 确认 WSL 平台还在

以**管理员身份**打开 PowerShell：

```powershell
wsl --status
wsl -l -v
```

正常应列出分发版（如 `docker-desktop`）。
若报「WLS 未安装 / 找不到」，说明 WSL 平台被删了，重装：

```powershell
# 商店通道不通时用离线包：
Invoke-WebRequest -Uri https://github.com/microsoft/WSL/releases/download/3.0.3/wsl.3.0.3.0.x64.msi -OutFile $env:TEMP\wsl.msi
Start-Process msiexec.exe -ArgumentList '/i',$env:TEMP\wsl.msi,'/quiet' -Wait
# 装完必须重启
Restart-Computer
```

### 步骤 4 — 启动 Docker Desktop

```powershell
Start-Process "C:\Program Files\Docker\Docker\Docker Desktop.exe"
```

或 `Win` 键搜 Docker Desktop 直接打开。
**首次启动要等 30–90 秒**，判断依据：托盘鲸鱼图标**停止动画、变成实心**。

### 步骤 5 — 验证

```powershell
docker version                                  # Client 与 Server 都要出来
docker info --format '{{.ServerVersion}} / {{.OSType}}'
docker run --rm hello-world                     # 应打印 Hello from Docker!
docker compose version
```

全部通过后再拉起项目：

```bash
export PATH="$PATH:/c/Program Files/Docker/Docker/resources/bin"   # Git Bash 里必须加
cd <项目目录>/deploy
docker compose up -d
docker ps --format '{{.Names}}\t{{.Status}}\t{{.Ports}}'
python3 verify.py --host 127.0.0.1 --port 8000     # 明文冒烟
./deploy.sh --tls                                   # 带 TLS 网关 + 反向鉴权第 4 步
```

> 数据卷 `mysql_data` 还在，容器重建**不会丢数据**。

---

## 路线 B（仅用于验证 Docker 本身）：切换到 Windows 容器后端

⚠️ **先看这段再决定**：本项目 Dockerfile 是 `FROM ubuntu:22.04`，MySQL 也是 Linux 镜像，
**Windows 容器后端跑不了它们**。这条路线只能证明"Docker 能起来"，**不能跑本项目**。
它只适合在你实在不想放行 `wsl.exe`、又想确认 Docker 安装是否完好时使用。

步骤：

1. 托盘右键 Docker 鲸鱼图标 → **Switch to Windows containers...**（切换至 Windows 容器）
   或：Docker Desktop → Settings → **General** → 取消勾选 **Use the WSL 2 based engine**
2. 等它重启后，托盘应显示 "Windows containers"
3. 验证：

```powershell
docker version
docker info --format '{{.OSType}}'     # 应返回 windows
docker run --rm mcr.microsoft.com/windows/nanoserver:ltsc2022 cmd /c echo ok
```

4. 想跑项目时再切回来：**Switch to Linux containers...**（仍需要 WSL2 → 回到路线 A）

---

## 故障对照表

| 现象 | 原因 | 处理 |
|---|---|---|
| `open //./pipe/dockerDesktopLinuxEngine: The system cannot find the file specified` | 引擎没起来 | 走路线 A 步骤 1→5 |
| `wsl.exe` 被安全策略拦（PROGRAM BLOCKED BY SECURITY POLICY） | 命令黑名单 | 安全中心 → 命令安全 → 程序黑名单放行 |
| wsl.exe 成功执行但 Docker 仍报错 | WSL 分发未注册 | `wsl --shutdown` 后重启 Docker Desktop |
| `Docker Desktop is unable to start` | WSL 平台缺失 | 装 `wsl.3.0.3.0.x64.msi` 并重启系统 |
| Docker 起来了但拉镜像超时 | Docker Hub 不通 | `~/.docker/daemon.json` 配 registry-mirrors（本机已配：`docker.1panel.live`、`docker.m.daocloud.io`、`docker.1ms.run`） |
| `command -v docker` 找不到 | Git Bash PATH 没带 | `export PATH="$PATH:/c/Program Files/Docker/Docker/resources/bin"` |
| 鲸鱼图标一直转 | 引擎反复重启 | `wsl --shutdown` → Docker Desktop 托盘 → **Troubleshoot → Restart** |

---

## 一条命令快速体检

以管理员 PowerShell 运行，输出应全部非空：

```powershell
wsl --status 2>&1 | Select-Object -First 3
& "C:\Program Files\Docker\Docker\resources\bin\docker.exe" version --format '{{.Server.Version}}'
& "C:\Program Files\Docker\Docker\resources\bin\docker.exe" ps --format '{{.Names}}'
```

只要 `docker version` 的 Server 段出版本号，就可以 `cd deploy && docker compose up -d` 了。
