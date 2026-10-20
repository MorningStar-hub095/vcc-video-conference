# 方案 B：虚拟机裸机部署 + mTLS 改造

目标环境：VMware 里的 Ubuntu 虚拟机（**不使用容器**，全部依赖 `apt install`）。
网关层终结 TLS，**不改动一行项目源码**：服务端进程继续只监听 `127.0.0.1:8000`，
由 nginx 的 `stream` 模块在 `8443` 上做 mTLS，验证通过后转发明文到后端。

```
Qt 客户端 ──127.0.0.1:8000──> stunnel(本机) ══TLS1.2/1.3 + 客户端证书══> nginx:8443 ──> 127.0.0.1:8000
                                                                              │
                                                               CRL:8080 ──────┘（发布吊销列表）
```

> 本文所有脚本都在 `deploy/bare/tls/` 下，且已在本地 Docker 实验室里用**同一份 nginx 配置**实测通过，
> 实测证据见文末「演练实录」。

---

## 0. 一句话总览

| 项 | 改造前（方案 A） | 改造后（方案 B） |
|---|---|---|
| 服务端证书 | 自签 CA 签发 | **公共可信 CA**（Let's Encrypt / 商业 CA） |
| 客户端证书 | 所有人共用一张 | **每人一套私钥 + 证书**，证书内含姓名/工号/邮箱 |
| 原自签 CA | 又签服务端又签客户端 | **降级为客户端专用根 CA**（只签客户端） |
| 吊销 | 无 | **CRL**，网关侧强校验，吊销后下一次握手即失效 |
| 部署形态 | Docker Compose | **裸机 + apt**，nginx stream 网关 |

---

## 1. 安装（裸机，全部 apt）

```bash
sudo apt-get update
sudo apt-get install -y openssl nginx stunnel4 certbot netcat-openbsd rsync

# nginx 必须带 stream_ssl 模块（Ubuntu 官方包自带）
nginx -V 2>&1 | grep -q with-stream_ssl_module && echo OK
```

完整安装脚本（含目录创建、权限、配置部署、定时器）：

```bash
sudo ./install-tls.sh --crl-url http://<网关IP>:8080/vcc-client-ca.crl
```

---

## 2. 存放目录与文件权限

### 2.1 目录结构

```
/etc/vcc-tls/                       # 网关只读的发布目录（证书/CRL）
├── server/
│   ├── fullchain.pem                  # 服务端证书链（公共 CA 签发）
│   └── privkey.pem                    # 服务端私钥
└── client-ca/
    ├── chain.crt                      # 客户端信任链 = 签发CA + 根CA
    ├── issuing-ca.crt                 # 签发 CA（中间 CA）
    ├── root-ca.crt                    # 根 CA（原自签 CA 降级而来）
    └── crl.pem                        # CRL（签发CA CRL + 根CA CRL 合并）

/opt/vcc-ca/                        # CA 状态与私钥（高敏感）
├── crl-url.txt                        # CRL 分发地址
├── root/                              # 根 CA（生产环境应离线保存）
│   ├── ca.crt  ca.key  index.txt  serial  crlnumber  openssl.cnf
├── intermediate/                      # 签发 CA（真正签用户证书）
│   ├── ica.crt ica.key index.txt serial crlnumber openssl.cnf
│   ├── inventory.csv                  # 台账：序列号/UID/姓名/邮箱/有效期/指纹/状态
│   ├── users/<uid>/{user.crt,user.key,user.csr,ext.cnf}
│   └── newcerts/<serial>.pem          # 签发存档（吊销时要用）
└── distribute/<uid>.p12               # 待分发给本人的打包文件

/var/www/html/vcc-client-ca.crl     # CRL 的 HTTP 副本（分发点）
```

### 2.2 权限要求（**必须照做**）

| 路径 | 权限 | 属主 | 理由 |
|---|---|---|---|
| `/opt/vcc-ca/` | `0700` | root | CA 私钥所在目录，禁止其他用户遍历 |
| `/opt/vcc-ca/root/ca.key` | `0400` | root | 根 CA 私钥；**生产环境应离线保存、从网关机删除** |
| `/opt/vcc-ca/intermediate/ica.key` | `0600` | root | 签发 CA 私钥 |
| `/opt/vcc-ca/intermediate/users/<uid>/` | `0700` | root | 每人私钥目录 |
| `.../users/<uid>/user.key` | `0600` | root | 用户私钥（分发后可 `--no-keep` 销毁服务端副本） |
| `/etc/vcc-tls/server/privkey.pem` | `0600` | root | 服务端私钥，nginx master 以 root 读取 |
| `/etc/vcc-tls/server/fullchain.pem` | `0644` | root | 证书链公开 |
| `/etc/vcc-tls/client-ca/*.crt` | `0644` | root | 信任链公开 |
| `/etc/vcc-tls/client-ca/crl.pem` | `0644` | root | CRL 公开 |
| `/opt/vcc-ca/intermediate/inventory.csv` | `0640` | root | 台账含个人信息，不对外 |

一键对齐（脚本里已内置，手动复核用）：

```bash
chmod 0700 /opt/vcc-ca /opt/vcc-ca/root /opt/vcc-ca/intermediate
chmod 0400 /opt/vcc-ca/root/ca.key
chmod 0600 /opt/vcc-ca/intermediate/ica.key /etc/vcc-tls/server/privkey.pem
chmod 0644 /etc/vcc-tls/server/fullchain.pem /etc/vcc-tls/client-ca/*.crt /etc/vcc-tls/client-ca/crl.pem
chmod 0640 /opt/vcc-ca/intermediate/inventory.csv
```

---

## 3. 改造点一：服务端证书改用公共可信 CA

### 3.1 为什么

- 客户端（stunnel）直接用**系统信任库**校验服务端，不再需要下发、安装自签根 CA；
- 杜绝中间人：服务端身份由公共 CA 背书；
- 服务端证书到期自动续期（certbot.timer）。

### 3.2 申请与部署

三种方式，脚本 `server-cert-acme.sh` 都支持：

```bash
# A. DNS-01（推荐，不需要公网 80 端口，支持泛域名）
sudo ./server-cert-acme.sh --domain meet.example.com --email ops@example.com --dns-plugin dns_aliyun

# B. 手工 DNS-01（任何 DNS 服务商）
sudo ./server-cert-acme.sh --domain meet.example.com --email ops@example.com --manual
#   → 脚本暂停，去 DNS 控制台加 _acme-challenge TXT 记录，回车继续

# C. 自带商业 CA 证书
sudo ./server-cert-acme.sh --import-cert /tmp/fullchain.pem --import-key /tmp/privkey.pem
```

脚本会：
1. 把 `fullchain.pem`(0644) / `privkey.pem`(0600) 落到 `/etc/vcc-tls/server/`；
2. 写入 certbot 续期钩子 `/etc/letsencrypt/renewal-hooks/deploy/vcc-tls.sh`
   （续期后自动同步到网关目录并 `systemctl reload nginx`）；
3. 用系统信任库做一次 `openssl verify` 自检；
4. reload 网关。

### 3.3 服务端配置片段（完整）

`/etc/nginx/nginx.conf` —— **`stream` 段必须在主上下文**，不能放 `conf.d`：

```nginx
user www-data;
worker_processes auto;
pid /run/nginx.pid;
error_log /var/log/nginx/error.log warn;

events { worker_connections 4096; }

# ---------- HTTP：仅用于发布 CRL ----------
http {
    include      /etc/nginx/mime.types;
    default_type application/octet-stream;
    access_log   /var/log/nginx/crl-access.log;

    server {
        listen 8080;
        server_name _;

        # CRL 分发点（与证书里 crlDistributionPoints 的 URI 保持一致）
        location = /vcc-client-ca.crl {
            alias /etc/vcc-tls/client-ca/crl.pem;
            default_type application/pkcs-crl;
            add_header Cache-Control "public, max-age=3600";
        }

        # 客户端 CA 信任链（供客户端导入，只读无害）
        location = /vcc-client-ca-chain.crt {
            alias /etc/vcc-tls/client-ca/chain.crt;
            default_type application/x-x509-ca-cert;
        }

        location / { return 404; }
    }
}

# ---------- STREAM：业务 mTLS 网关 ----------
stream {
    log_format vcc '$remote_addr [$time_local] '
                      'tls=$ssl_protocol/$ssl_cipher '
                      'client=$ssl_client_s_dn '
                      'verify=$ssl_client_verify '
                      'up=$upstream_addr';
    access_log /var/log/nginx/vcc-stream.log vcc;

    upstream vcc_backend {
        server 127.0.0.1:8000 max_fails=0;   # 后端只监听回环
    }

    # 可选：来源 IP 白名单（叠加在 mTLS 之上的纵深防御）
    # allow 192.168.1.0/24;
    # deny  all;

    server {
        listen 8443 ssl;
        listen [::]:8443 ssl;

        # ---- 服务端证书：公共可信 CA ----
        ssl_certificate     /etc/vcc-tls/server/fullchain.pem;
        ssl_certificate_key /etc/vcc-tls/server/privkey.pem;

        ssl_protocols TLSv1.2 TLSv1.3;
        ssl_ciphers ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-RSA-AES256-GCM-SHA384:HIGH:!aNULL:!MD5:!3DES;
        ssl_prefer_server_ciphers on;
        ssl_session_cache shared:VCC_SSL:10m;
        ssl_session_timeout 10m;
        ssl_session_tickets off;

        # ---- 客户端证书鉴权：自有客户端 CA ----
        ssl_client_certificate /etc/vcc-tls/client-ca/chain.crt;  # 签发CA + 根CA
        ssl_verify_client      on;      # 没有有效证书直接握手失败
        ssl_verify_depth       3;       # 用户证书 → 签发CA → 根CA

        # ---- CRL 吊销校验 ----
        ssl_crl /etc/vcc-tls/client-ca/crl.pem;

        proxy_pass vcc_backend;
        proxy_connect_timeout 5s;
        proxy_timeout 24h;              # 音视频会议长连接
    }
}
```

对应的完整文件：`nginx-vcc-tls.conf`。

> **两个坑**
> 1. `ssl_crl` 只在**配置加载时**读取文件内容。吊销后必须 `systemctl reload nginx`
>    （`revoke-user.sh` / `crl-renew.sh` 已自动执行）。
> 2. CRL 的 `nextUpdate` 一旦过期，**所有客户端都会被拒**。必须配定时续签（见 §5.4）。

---

## 4. 改造点二：客户端证书按人签发

### 4.1 原自签 CA 降级

原来的自签 CA（`deploy/certs/ca.crt` + `ca.key`）**不再签发任何服务端证书**，
只作为客户端证书体系的**根 CA** 复用：

```bash
sudo ./ca-init.sh --reuse-root /path/to/deploy/certs \
                  --crl-url http://<网关IP>:8080/vcc-client-ca.crl
```

产出：根 CA 复用 → 新建**签发 CA（中间 CA，pathlen:0）** → 发布 `chain.crt`（签发CA+根CA）。
日常签发只动中间 CA，根 CA 私钥可以离线封存。

### 4.2 证书里的使用者标识

`subject`（DN）与 `subjectAltName` 同时体现：

```
subject=O=VCCMeeting, OU=Client, CN=张三, UID=u001, emailAddress=zhangsan@example.com
subjectAltName = email:zhangsan@example.com
extendedKeyUsage = clientAuth
crlDistributionPoints = URI:http://<网关IP>:8080/vcc-client-ca.crl
```

网关日志里能直接看到是谁在连：

```
client=emailAddress=zhangsan@example.com,CN=张三,OU=Client,O=VCCMeeting verify=SUCCESS
```

### 4.3 签发流程

```bash
sudo ./issue-user.sh --uid u001 --name 张三 --email zhangsan@example.com --days 365
```

输出：

```
序列号   : 1000
有效期   : Oct 13 08:44:59 2026 GMT → Oct 13 08:44:59 2027 GMT
指纹     : 23:75:83:...
证书     : /opt/vcc-ca/intermediate/users/u001/user.crt
私钥     : /opt/vcc-ca/intermediate/users/u001/user.key
分发包   : /opt/vcc-ca/distribute/u001.p12
一次性口令: WlBg0vUvykDeYRgl   ← 必须通过独立渠道告知本人
```

所有签发记录自动进台账 `inventory.csv`：

```csv
serial,uid,name,email,issued,expires,fingerprint,status
1000,u001,张三,zhangsan@example.com,Oct 13 08:44:59 2026 GMT,Oct 13 08:44:59 2027 GMT,23:75:83:...,issued
```

### 4.4 分发流程

1. **打包**：脚本自动生成 `u001.p12`（含用户证书 + 私钥 + 信任链），口令为随机 16 位一次性口令；
2. **双通道交付**：`p12` 走一个渠道（邮件/IM），**口令必须走另一个渠道**（当面 / 加密 IM / 电话）。
   两者同一渠道发送等于没有口令；
3. **本人导入**：
   - Windows：双击 p12 → 存储位置「个人」→ 输入口令；stunnel 里 `cert = Computer\Personal`；
   - Linux：直接用 `user.crt` + `user.key`，见 `stunnel-client-user.conf.template`；
4. **服务端副本销毁**（可选，推荐）：加 `--no-keep`，打包后用 `shred` 销毁服务端上的用户私钥副本，
   做到「私钥只有本人有」；
5. **登记**：台账 + 指纹存档，后续吊销靠序列号定位。

### 4.5 吊销流程

```bash
sudo ./revoke-user.sh --uid u001                      # 默认 reason=keyCompromise
sudo ./revoke-user.sh --serial 1000 --reason cessationOfOperation
```

脚本自动完成四件事：
1. `openssl ca -revoke` 写进 `index.txt`；
2. 更新 `inventory.csv` 状态为 `revoked:<时间戳>`；
3. **重新生成并发布 CRL**（签发 CA CRL + 根 CA CRL 合并）；
4. `systemctl reload nginx`（网关立即生效）。

可选 reason：`unspecified | keyCompromise | CACompromise | cessationOfOperation | superseded | certificateHold`。
**人员离职/设备丢失一律用 `keyCompromise`**。

> 私钥泄露属于 `keyCompromise`；如果同一人重新入职，请**换新 UID 重新签发**，
> 不要复用旧 UID（`unique_subject = no` 允许同 DN 多次签发，但台账会按最新一条定位）。

---

## 5. 改造点三：CRL

### 5.1 生成

```bash
sudo ./crl-renew.sh
```

`crl-renew.sh` → `publish_crl()`：

```bash
openssl ca -config /opt/vcc-ca/root/openssl.cnf        -gencrl -crldays 30 -out root/crl.pem
openssl ca -config /opt/vcc-ca/intermediate/openssl.cnf -gencrl -crldays 30 -out intermediate/crl.pem
cat intermediate/crl.pem root/crl.pem > /etc/vcc-tls/client-ca/crl.pem
chmod 0644 /etc/vcc-tls/client-ca/crl.pem
```

> **为什么合并两个 CRL**：nginx 校验客户端证书链时，链上每个 CA 都要能拿到对应 CRL，
> 否则报 `unable to get certificate CRL`，会导致**所有**客户端连不上。

### 5.2 分发方式与访问地址

| 方式 | 地址 | 说明 |
|---|---|---|
| HTTP（主） | `http://<网关IP>:8080/vcc-client-ca.crl` | nginx http 块发布，`alias` 指向 `crl.pem` |
| 证书内 CDP | 同址 | 每张用户证书都写了 `crlDistributionPoints = URI:...`，任何支持 CDP 的客户端都能自动定位 |
| 内部同步 | `/var/www/html/vcc-client-ca.crl` | 多网关时由 rsync / 配置管理统一下发 |

8080 是**只读公开**内容（CRL 本身不含敏感信息），但建议只监听内网或用防火墙限制来源。

### 5.3 服务端启用

```nginx
ssl_crl /etc/vcc-tls/client-ca/crl.pem;   # 配合 ssl_verify_client on
```

### 5.4 更新周期

- CRL 有效期 **30 天**（`CRL_DAYS=30`）；
- systemd timer **每天 03:17** 续签一次（`OnCalendar=*-*-* 03:17:00`，`RandomizedDelaySec=300`），
  实际就是不断刷新 `nextUpdate`，保证离过期永远有 29 天余量；
- 另有**事件驱动**：每次 `revoke-user.sh` 立刻重新生成 CRL + reload，**不需要等定时器**。

```bash
sudo systemctl enable --now vcc-crl.timer
systemctl list-timers vcc-crl*
```

定时器内容（`systemd/vcc-crl.timer` + `.service`）：

```ini
[Timer]
OnCalendar=*-*-* 03:17:00
RandomizedDelaySec=300
Persistent=true
```

```ini
[Service]
Type=oneshot
Environment=CA_DIR=/opt/vcc-ca
Environment=TLS_DIR=/etc/vcc-tls
Environment=CRL_PUB_DIR=/var/www/html
ExecStart=/opt/vcc-ca/crl-renew.sh
User=root
```

---

## 6. 验证步骤

### 6.1 一键验收

```bash
sudo ./verify-mtls.sh \
     --host 127.0.0.1 --port 8443 \
     --good    /opt/vcc-ca/intermediate/users/u001 \
     --revoked /opt/vcc-ca/intermediate/users/u002 \
     --crl-url http://127.0.0.1:8080/vcc-client-ca.crl
```

退出码 `0` = 三项断言全过。脚本检查：正常证书能握手并收发数据 / 已吊销证书被拒 /
CRL 含该序列号 / CRL 分发点可访问 / 网关日志判定。

### 6.2 手工验证（逐条）

**① 正常证书 —— 握手 + 注册 + 登录成功**

```bash
python3 deploy/verify.py --host 127.0.0.1 --port 8443 --tls \
  --ca /etc/ssl/certs/ca-certificates.crt \
  --cert /opt/vcc-ca/intermediate/users/u001/user.crt \
  --key  /opt/vcc-ca/intermediate/users/u001/user.key
# 期望：
# ✓ [1/4] TLS 握手成功 (TLSv1.3)
# ✓ [2/4] 注册成功
# ✓ [3/4] 登录成功
# ✓ [4/4] 无证书连接被网关拒绝（mTLS 鉴权生效）
```

**② 已吊销证书 —— 被拒绝**

```bash
sudo ./revoke-user.sh --uid u002
python3 deploy/verify.py --host 127.0.0.1 --port 8443 --tls \
  --ca /etc/ssl/certs/ca-certificates.crt \
  --cert /opt/vcc-ca/intermediate/users/u002/user.crt \
  --key  /opt/vcc-ca/intermediate/users/u002/user.key
# 期望：握手后立刻被断开，注册/登录拿不到任何响应（非 0 退出）
```

**③ 以网关日志为最终判据（重要）**

TLS 1.3 下客户端会在收到服务端 alert **之前**就认为握手已完成，
所以「客户端说自己 OK」不算数，必须看网关侧的 `$ssl_client_verify`：

```bash
sudo tail -5 /var/log/nginx/vcc-stream.log
```

期望看到：

```
... client=emailAddress=zhangsan@example.com,CN=张三,... verify=SUCCESS up=127.0.0.1:8000
... client=emailAddress=lisi@example.com,CN=李四,...     verify=FAILED:certificate revoked up=-
```

`up=-` 表示**没有连到后端**，会话在网关就被掐掉。

**④ CRL 分发点**

```bash
curl -s -o /tmp/crl.pem -w 'HTTP %{http_code} %{size_download}B\n' \
     http://127.0.0.1:8080/vcc-client-ca.crl
openssl crl -in /tmp/crl.pem -noout -issuer -nextupdate
openssl crl -in /tmp/crl.pem -noout -text | grep -A1 'Serial Number'
```

**⑤ 服务端证书是公共 CA**

```bash
openssl verify -CAfile /etc/ssl/certs/ca-certificates.crt /etc/vcc-tls/server/fullchain.pem
openssl x509 -in /etc/vcc-tls/server/fullchain.pem -noout -issuer -dates
```

### 6.3 演练实录（本地 Docker 实测同一份 nginx 配置）

因虚拟机尚未安装 ssh/nginx，用 nginx:1.27 容器加载**裸机同款配置**做了完整演练
（`lab/run-lab.sh`，与裸机配置的差异只有转发目标和运行用户两处）：

```
--- A. 张三(u001) 正常证书 ---
✓ [1/4] TLS 握手成功 127.0.0.1:8443 (TLSv1.3)
✓ [2/4] 注册成功 (tel=13900000000)
✓ [3/4] 登录成功 (userid=12, result=2)
✓ [4/4] 无证书连接被网关拒绝（mTLS 鉴权生效）

--- B. 李四(u002) 已吊销证书 ---
✗ [2/4] 注册无响应   ← 握手后即被网关切断

--- B2. 网关日志（唯一权威判据）---
... client=emailAddress=zhangsan@example.com,CN=张三,... verify=SUCCESS up=172.18.0.2:8000
... client=-                                             verify=NONE    up=-   ← 无证书
... client=emailAddress=lisi@example.com,CN=李四,...     verify=FAILED:certificate revoked up=-

--- C. CRL ---
Serial Number: 1001      （u002 序列号 1001，已在吊销列表）
Next Update: Nov 12 09:31:48 2026 GMT

--- D. CRL HTTP 分发点 ---
HTTP 200  2007 bytes
issuer=O=VCCMeeting, OU=Client-PKI, CN=VCC-Client-Issuing-CA
```

通用验收脚本同样全绿：`verify-mtls.sh` 退出码 `0`
（正常证书 `TLSv1.3 握手成功，连接保持`；已吊销证书 `握手后即刻被服务端关闭`）。

> 排障提示：演练中曾出现「客户端显示握手成功」的假象——原因是 TLS 1.3 客户端
> 在收到服务端 alert 前就完成握手。**判断吊销是否生效，一律以网关日志为准。**

---

## 7. 回滚到原自签 CA 方案

### 7.1 裸机（方案 B → 方案 A 的自签配置）

`install-tls.sh` 会自动备份原配置为 `/etc/nginx/nginx.conf.orig`，回滚只需三分钟：

```bash
# 1) 备份当前 mTLS 配置（留作排查，不要直接删）
sudo mkdir -p /etc/vcc-tls.rollback.$(date +%F)
sudo cp -a /etc/vcc-tls /etc/vcc-tls.rollback.$(date +%F)/
sudo cp -a /etc/nginx/nginx.conf /etc/vcc-tls.rollback.$(date +%F)/nginx.conf.mtls

# 2) 恢复自签方案配置（三选一）
sudo cp -a /etc/nginx/nginx.conf.orig /etc/nginx/nginx.conf          # 有备份
# 或手工改：ssl_certificate 指回自签 server.crt、ssl_crl 行整行删除
sudo sed -i '/ssl_crl/d' /etc/nginx/nginx.conf

# 3) 校验并生效
sudo nginx -t && sudo systemctl reload nginx

# 4) 停掉 CRL 定时器（保留 .service/.timer 文件，只是不再触发）
sudo systemctl disable --now vcc-crl.timer

# 5) 回滚后验收
python3 deploy/verify.py --host 127.0.0.1 --port 8443 --tls \
  --ca  deploy/certs/ca.crt \
  --cert deploy/certs/client.crt --key deploy/certs/client.key
```

回滚后**客户端侧**也要同步回退：stunnel 的 `CAfile` 从系统信任库改回自签 `ca.crt`。

### 7.2 回到 Docker 方案 A（更彻底的回滚）

```bash
git checkout v2026.10.13-3            # 方案 A 打过 tag 的提交
cd deploy
docker compose -f docker-compose.tls.yml up -d
./deploy.sh --tls
```

CA 与证书资产全部保留在 `deploy/certs/`，删除的只是 nginx 配置与定时器，
**私钥与台账始终不丢**，因此回滚不会造成证书体系不可恢复。

---

## 8. 文件清单

| 文件 | 作用 |
|---|---|
| `ca-lib.sh` | 路径/权限/CRL 发布/网关重载 公共库 |
| `ca-init.sh` | 客户端 CA 初始化（支持 `--reuse-root` 复用原自签 CA 降级） |
| `issue-user.sh` | 按人签发（姓名/工号/邮箱，p12 打包，台账登记） |
| `revoke-user.sh` | 吊销 + 立即重发 CRL + reload |
| `crl-renew.sh` | CRL 定期续签（供 systemd timer 调用） |
| `server-cert-acme.sh` | 公共可信 CA 服务端证书申请/导入 + 续期钩子 |
| `install-tls.sh` | 裸机一键安装（apt + 目录 + 权限 + 配置 + 定时器） |
| `nginx-vcc-tls.conf` | 网关完整配置（stream mTLS + CRL 发布） |
| `stunnel-server.conf` | 无 stream_ssl 模块时的 stunnel 回退方案 |
| `stunnel-client-user.conf.template` | 客户端（每人一份）接入模板 |
| `systemd/vcc-crl.{service,timer}` | CRL 每日续签 |
| `verify-mtls.sh` | 验收脚本（正常/吊销/CRL 三项断言） |
| `lab/` | 本地 Docker 演练（复用同一份 nginx 配置） |

---

## 9. 生产环境加固建议

1. **根 CA 私钥离线**：`/opt/vcc-ca/root/ca.key` 只在离线机器上保存，网关机只留 `chain.crt`；
2. **CRL 监控**：对 `vcc-crl.timer` 加监控，CRL 过期 = 全量断连；
3. **台账备份**：`inventory.csv` + `index.txt` 定期备份，丢了就无法精确定位吊销对象；
4. **考虑 OCSP**：人多了之后 CRL 会变大，可平滑演进到 OCSP stapling；
5. **业务层二次鉴权**：mTLS 只证明「证书有效」，账号密码/bcrypt 登录依然保留，二者是叠加关系；
6. **防火墙**：只放行 `8443`（业务）与内网 `8080`（CRL），`8000` 必须绑 `127.0.0.1`。
