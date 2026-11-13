FROM devkitpro/devkitarm:latest
RUN apt-get update && apt-get install -y --no-install-recommends build-essential g++ bison flex ninja-build pkg-config libpng-dev python3 python3-pip python3-venv git ca-certificates cmake && rm -rf /var/lib/apt/lists/*
RUN python3 -m pip install --break-system-packages "meson>=1.12" 2>/dev/null || python3 -m pip install "meson>=1.12"
