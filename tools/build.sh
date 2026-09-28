#!/usr/bin/env bash
# Compila o hiddriver.xex no Linux usando a imagem Docker do XDK do CollectionUI
# (receita em ~/Documentos/CollectionUI/toolchain). Saída: build/bin/hiddriver.xex
#
# A imagem só traz um subconjunto das libs do XDK; o xkelib pede xav.lib, então a pasta
# de libs inteira do XDK extraído é montada por cima, só leitura.
#
# O container roda como root porque o wine recusa prefixo de outro dono; a posse dos
# arquivos é devolvida no fim (mesma razão do build.sh do CollectionUI).
set -euo pipefail

IMAGEM=${IMAGEM:-collectionui-xdk:light}
XDK_DIR=${XDK_DIR:-$HOME/Documentos/CollectionUI/sdk/XDK}
RAIZ=$(cd "$(dirname "$0")/.." && pwd)

[ -d "$XDK_DIR/lib/xbox" ] || { echo "XDK não encontrado em $XDK_DIR (defina XDK_DIR)"; exit 1; }

docker run --rm -v "$RAIZ":/app -v "$XDK_DIR/lib/xbox":/xdk/lib/xbox:ro -w /app "$IMAGEM" make "$@"
docker run --rm -v "$RAIZ":/app "$IMAGEM" chown -R "$(id -u):$(id -g)" /app

# Guarda: o bit 0x8 das image flags ("XGD2 media only") faz o console recusar o .xex
# fora de um disco de jogo. Ele aparece se <dvdxgd2/> entrar no <mediatypes> do xex.xml.
for xex in $(find "$RAIZ/build" -name '*.xex' -newer "$RAIZ/Makefile" 2>/dev/null); do
	python3 - "$xex" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read()
so = struct.unpack('>I', d[16:20])[0]
flags = struct.unpack('>I', d[so + 0x10C:so + 0x110])[0]
if flags & 0x8:
    print(f"ERRO: {sys.argv[1]} saiu com image flags {flags:#x} (so disco XGD2); tire <dvdxgd2/> do xex.xml")
    sys.exit(1)
PY
done
