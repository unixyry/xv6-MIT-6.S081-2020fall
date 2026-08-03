FROM debian:bullseye-slim

ARG USERNAME=yyq
ARG USER_UID=1000
ARG USER_GID=1000
ARG APT_MIRROR=mirrors.ustc.edu.cn

# 使用可覆盖的 Debian 镜像地址，改善国内网络下载速度。
RUN sed -i \
      -e "s|deb.debian.org|${APT_MIRROR}|g" \
      -e "s|security.debian.org|${APT_MIRROR}|g" \
      /etc/apt/sources.list \
    && apt-get \
      -o Acquire::Retries=5 \
      -o Acquire::ForceIPv4=true \
      -o Acquire::http::Timeout=60 \
      -o Acquire::http::Pipeline-Depth=0 \
      update \
    && DEBIAN_FRONTEND=noninteractive apt-get \
      -o Acquire::Retries=5 \
      -o Acquire::ForceIPv4=true \
      -o Acquire::http::Timeout=60 \
      -o Acquire::http::Pipeline-Depth=0 \
      install -y --no-install-recommends \
        binutils-riscv64-linux-gnu \
        build-essential \
        ca-certificates \
        gcc-riscv64-linux-gnu \
        gdb-multiarch \
        git \
        python3 \
        qemu-system-misc \
    && rm -rf /var/lib/apt/lists/*

# Ubuntu 24.04 reserves UID/GID 1000 for its default ubuntu user. Replace it
# so files created in the bind mount belong to the host user by default.
RUN if id -u ubuntu >/dev/null 2>&1; then userdel -r ubuntu; fi \
    && if ! getent group "${USER_GID}" >/dev/null; then \
         groupadd --gid "${USER_GID}" "${USERNAME}"; \
       fi \
    && useradd --uid "${USER_UID}" --gid "${USER_GID}" \
         --create-home --shell /bin/bash "${USERNAME}" \
    && mkdir -p /workspace \
    && chown "${USER_UID}:${USER_GID}" /workspace

WORKDIR /workspace
USER ${USERNAME}

CMD ["/bin/bash"]
