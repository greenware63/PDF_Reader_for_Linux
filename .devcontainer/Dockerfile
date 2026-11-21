# Dev Container for Linux PDF Reader Workstation
# Matches the Ubuntu 24.04 CI runner environment from .github/workflows/ci.yml

FROM ubuntu:24.04

# Prevent interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install dependencies:
# 1. Base CI package set (from .github/workflows/ci.yml):
#    - build-essential, cmake, ninja-build: C++20 compiler and build system
#    - qt6-base-dev, libgl1-mesa-dev, libxkbcommon-dev: Qt 6 base development headers and OpenGL/XKB libraries
#    - qpdf, poppler-utils, mupdf-tools: external engine comparison tools (mutool, pdftoppm, pdfsig, qpdf)
# 2. Additional development and testing tools:
#    - zlib1g-dev: required for test_redaction (RedactionTestSuite) independent inflate verification (pre-installed on GitHub Actions runner image)
#    - git: version control, commit hash verification, and CMake FetchContent
#    - python3: execution of verification and validation scripts (e.g. scripts/validate_matrix.py)
#    - gdb: native debugging inside CLion / container
#    - clang, llvm: llvm-symbolizer for AddressSanitizer, ThreadSanitizer, and UBSan stack traces
#    - sudo, ca-certificates, curl: container management and SSL certificates
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    qt6-base-dev \
    libgl1-mesa-dev \
    libxkbcommon-dev \
    qpdf \
    poppler-utils \
    mupdf-tools \
    zlib1g-dev \
    git \
    python3 \
    gdb \
    clang \
    llvm \
    sudo \
    ca-certificates \
    curl \
    && rm -rf /var/lib/apt/lists/*

# Configure passwordless sudo for the default non-root user 'ubuntu' (UID/GID 1000)
RUN echo "ubuntu ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/ubuntu \
    && chmod 0440 /etc/sudoers.d/ubuntu

# Pre-create container-local build root with correct ownership
# (keeps build artifacts off the slower Windows 9p/virtiofs mount)
RUN mkdir -p /home/ubuntu/build /home/ubuntu/.cache \
    && chown -R ubuntu:ubuntu /home/ubuntu/build /home/ubuntu/.cache

# Default headless Qt platform and CI environment flag
ENV QT_QPA_PLATFORM=offscreen
ENV CI=1

USER ubuntu
WORKDIR /workspaces/PDF_Reader_for_Linux
