# devkitARM plus the two CIA tools it lacks (makerom, bannertool), built from source; Pillow and a
# font for the HOME Menu art (tools/cia_art.py).
FROM devkitpro/devkitarm:latest
RUN apt-get update && apt-get install -y --no-install-recommends build-essential cmake git ca-certificates python3 python3-pil fonts-dejavu-core && rm -rf /var/lib/apt/lists/*
RUN git clone --depth 1 --branch makerom-v0.18.4 https://github.com/3DSGuy/Project_CTR.git /tmp/ctr \
 && make -C /tmp/ctr/makerom deps && make -C /tmp/ctr/makerom \
 && find /tmp/ctr/makerom -name makerom -type f -perm -u+x -exec cp {} /usr/local/bin/makerom \; \
 && rm -rf /tmp/ctr
RUN git clone --depth 1 --recursive https://github.com/diasurgical/bannertool.git /tmp/bt \
 && cd /tmp/bt/source \
 && for f in pc/stb_image.c pc/stb_vorbis.c; do gcc -O2 -w -c -Iinclude -I. -I.. $f -o ${f%.c}.o; done \
 && g++ -O2 -w -std=gnu++11 -DVERSION_MAJOR=1 -DVERSION_MINOR=2 -DVERSION_MICRO=2 -I. -I../include main.cpp cmd.cpp 3ds/*.cpp pc/wav.cpp pc/*.o -o /usr/local/bin/bannertool \
 && cd / \
 && rm -rf /tmp/bt
RUN makerom 2>&1 | head -2; bannertool 2>&1 | head -2; test -x /usr/local/bin/makerom && test -x /usr/local/bin/bannertool
