#!/usr/bin/env bash
# Grava o report descriptor e 15 s de reports crus de um controle HID, achado pelo nome.
# Uso: sudo tools/capturar_bruto.sh "Ultimate C" port/logs/8bitdo_bruto.txt
set -euo pipefail
nome="${1:?nome (ou parte do nome) do controle}"
saida="${2:?arquivo de saida}"
uevent=$(grep -l "$nome" /sys/class/hidraw/*/device/uevent | head -1)
no=$(basename "$(dirname "$(dirname "$uevent")")")
{
	echo "no: $no"
	grep HID_NAME "$uevent"
	echo "descritor:"
	xxd -p "/sys/class/hidraw/$no/device/report_descriptor" | tr -d '\n'
	echo
	echo "reports (aperte botoes e mexa os analogicos por 15 s):"
} | tee "$saida"
timeout 15 xxd -c 32 "/dev/$no" | tee -a "$saida" || true
chown "${SUDO_UID:-$(id -u)}:${SUDO_GID:-$(id -g)}" "$saida"
