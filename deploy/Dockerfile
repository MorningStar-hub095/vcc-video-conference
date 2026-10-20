# ============================================================
# PlayerHallServer 服务端镜像（Linux / epoll / MySQL / bcrypt）
# 构建上下文：deploy/.staging/  （由 prepare.sh 生成，见该脚本注释）
#   .staging/PlayerHallServer/include/*.h
#   .staging/PlayerHallServer/src/*.cpp
#
# 关于密钥（P0-2 已修复）：源码 packdef.h 已改为运行期 _db_env() 读取
#   DB_NAME / DB_IP / DB_USER / DB_PWD 四个环境变量，
#   因此镜像里不再残留任何明文口令，密钥由 docker compose 的
#   environment 在运行期注入（见 docker-compose.yml）。
#   构建期不再需要 sed 注入，本文件也不再声明任何 DB_* ARG。
# ============================================================

# ---------- 阶段一：编译 ----------
FROM ubuntu:22.04 AS builder

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        g++ make libmysqlclient-dev libcrypt-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
# 构建上下文是 deploy/，源码由 prepare.sh 落在 deploy/.staging/ 下
COPY .staging/PlayerHallServer/include ./include
COPY .staging/PlayerHallServer/src    ./src
COPY health-probe.cpp                 ./health-probe.cpp

# 只保留 src 下的实现；根目录 main.cpp 是空桩，绝不能参与编译
# 另：确认源码已完成 P0-2 改造（口令走环境变量），否则构建期即报错提醒
RUN rm -f ./src/makefile \
 && rm -f ./src/*.hbiFEK \
 && (grep -q '_db_env' ./include/packdef.h \
     || { echo "packdef.h 未做环境变量改造，请先执行 python3 _patch_db_env.py"; exit 1; })

# -lcrypt 为 bcrypt_wrapper.cpp（crypt_r）必需；src/makefile 里漏了这一项
RUN g++ -std=c++11 -O2 -Wall -I./include \
        src/*.cpp -o /src/playerhallserver \
        -lpthread -lmysqlclient -lcrypt \
 && chmod +x /src/playerhallserver

# 业务级健康检查探针：healthcheck 用它替代 nc -z，
# 才能发现"端口在但 MySQL 断连 / 线程池阻塞"这类真故障。详见该文件头注释。
RUN g++ -std=c++11 -O2 -Wall -o /src/health-probe health-probe.cpp \
 && chmod +x /src/health-probe

# ---------- 阶段二：运行 ----------
FROM ubuntu:22.04

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        libmysqlclient21 libcrypt1 netcat-openbsd \
    && rm -rf /var/lib/apt/lists/* \
    && useradd -r -s /bin/false vcc

COPY --from=builder /src/playerhallserver /usr/local/bin/playerhallserver
COPY --from=builder /src/health-probe    /usr/local/bin/health-probe

USER vcc
EXPOSE 8000

# argv[1] 为监听端口，缺省 8000
ENTRYPOINT ["/usr/local/bin/playerhallserver"]
CMD ["8000"]
