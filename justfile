default: build

vendor-fetch:
    mkdir -p vendor
    curl -sL -o vendor/qrcodegen.c https://raw.githubusercontent.com/nayuki/QR-Code-generator/master/c/qrcodegen.c
    curl -sL -o vendor/qrcodegen.h https://raw.githubusercontent.com/nayuki/QR-Code-generator/master/c/qrcodegen.h
    curl -sL -o vendor/stb_image.h https://raw.githubusercontent.com/nothings/stb/master/stb_image.h
    curl -sL -o vendor/stb_image_write.h https://raw.githubusercontent.com/nothings/stb/master/stb_image_write.h

build:
    cc -Wall -Wextra -O2 -o qrgen main.c vendor/qrcodegen.c -lncurses -lm

clean:
    rm -f qrgen *.o

run: build
    ./qrgen
