#!/usr/bin/env bash
# Captura VID/PID, HID report descriptor e reports crus de um controle USB HID.
# Uso: sudo ./tools/capturar_controle.sh <nome>   (ex.: 8bitdo_ultimate_c)
# Saída: tools/capturas/<nome>/
set -euo pipefail
nome="${1:?informe um nome, ex.: 8bitdo_ultimate_c}"
out="$(dirname "$0")/capturas/$nome"
mkdir -p "$out"

# Pega o dispositivo HID USB (bus 0003) mais recente
dev=$(ls -dt /sys/bus/hid/devices/0003:* 2>/dev/null | head -1)
[ -n "$dev" ] || { echo "Nenhum HID USB encontrado. Plugue o controle em modo DirectInput."; exit 1; }
hidraw=$(ls "$dev/hidraw" | head -1)

grep -h HID_NAME "$dev/uevent" | tee "$out/info.txt"
basename "$dev" | tee -a "$out/info.txt"          # 0003:VVVV:PPPP.xxxx
cp "$dev/report_descriptor" "$out/report_descriptor.bin"
xxd "$out/report_descriptor.bin" > "$out/report_descriptor.hex"
lsusb -v -d "$(basename "$dev" | cut -d: -f2):$(basename "$dev" | cut -d: -f3 | cut -d. -f1)" > "$out/lsusb.txt" 2>/dev/null || true

gravar() {  # $1 = rótulo, $2 = segundos
  echo ">>> $1 (${2}s)"
  timeout "$2" cat "/dev/$hidraw" | xxd -c 64 -p > "$out/$1.hex" || true
}

echo "Vamos gravar cada entrada separadamente. Solte tudo entre as etapas."
read -rp "ENTER e NÃO toque em nada (repouso)..."; gravar repouso 2
for b in A B X Y LB RB LT RT BACK START L3 R3 HOME DPAD_UP DPAD_DOWN DPAD_LEFT DPAD_RIGHT; do
  read -rp "ENTER e aperte/segure $b ..."; gravar "btn_$b" 2
done
for a in LX_esq_dir LY_cima_baixo RX_esq_dir RY_cima_baixo; do
  read -rp "ENTER e mova o analógico ($a) até os extremos ..."; gravar "eixo_$a" 3
done
chown -R "${SUDO_USER:-$USER}" "$out"
echo "Pronto: $out"
