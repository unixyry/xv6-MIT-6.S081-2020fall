FROM ubuntu:24.04

ARG USERNAME=yyq
ARG USER_UID=1000
ARG USER_GID=1000

# xv6/6.S081: compiler, RISC-V cross-toolchain, QEMU, GDB and grade scripts.
# --no-install-recommends and apt cache cleanup keep the image relatively small.
RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
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
