# stunnel Windows 运行时放置目录

`pack_client.bat --with-stunnel` 会从本目录把 stunnel 二进制与其依赖 DLL 拷进客户端包
（`dist\VCC\stunnel\`）。本目录**默认不存在**，脚本缺失时会跳过二进制拷贝并给出告警，
其余产物（隧道配置 / 用户证书 / 一键启动脚本）照常生成。

## 取二进制

1. 到 stunnel 官网下载 Windows 安装包： https://www.stunnel.org/downloads.html
   （选 `stunnel-x.xx-win64-installer.exe`，本项目服务端是 TLS 网关，客户端版本不限）
2. 安装后，把安装目录下的这些文件复制到本目录：

```
stunnel.exe          ← 主程序
tstunnel.exe         ← 可选（命令行版）
libcrypto-*.dll
libssl-*.dll
zlib1.dll
libgcc_s_*.dll       ← 若有
libwinpthread-1.dll  ← 若有
```

典型安装位置：`C:\Program Files (x86)\stunnel\bin\`

复制命令（PowerShell，按实际路径改）：

```powershell
$src = "C:\Program Files (x86)\stunnel\bin"
New-Item -ItemType Directory -Force -Path "D:\vcc-video-conference\deploy\stunnel" | Out-Null
Copy-Item "$src\*" "D:\vcc-video-conference\deploy\stunnel\" -Force
```

## 注意

- 本目录**不要提交到仓库**：内含第三方二进制，且体积大。
  `.gitignore` 未忽略它是因为它是空的；如需忽略请自行在 `deploy/.gitignore` 追加 `stunnel/`。
- 校验服务端身份建议用**系统信任库**（服务端证书已改公共可信 CA），
  若打包时提供了 `certs\server-ca.pem` 则用它校验。
- 用户 p12 由签发脚本产出，口令必须**与分发渠道分离**告知本人。
