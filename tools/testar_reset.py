#!/usr/bin/env python3
"""Testa no PC se o 8BitDo cai quando é configurado sem que as strings tenham sido pedidas.

O Xbox 360 (kernel 17150) enumera assim: descritor do dispositivo, descritor de configuração e
SET_CONFIGURATION, sem pedir as strings. Na enumeração normal o Linux pede as strings (idioma,
fabricante, produto, número de série) antes de configurar. Num reset de porta (USBDEVFS_RESET)
ele relê os descritores e manda o SET_CONFIGURATION direto, sem as strings: a sequência do Xbox.

O script grava o tráfego USB do barramento (usbmon), reseta o 8BitDo e observa por 20 s se ele
sai do barramento e volta (como no Xbox) ou se continua conectado. Resultado em
port/logs/reset_<rotulo>.txt e port/logs/usbmon_<rotulo>.txt.

Uso: sudo port/.venv/bin/python -u tools/testar_reset.py <rotulo>
  ex.: sudo port/.venv/bin/python -u tools/testar_reset.py receptor
"""
import os
import subprocess
import sys
import threading
import time

import usb.core

VID = 0x2DC8


def listar():
    """8BitDo devices currently on the bus: {(bus, address): (pid, bcdDevice, product)}."""
    out = {}
    for d in usb.core.find(find_all=True, idVendor=VID):
        try:
            prod = d.product
        except Exception:
            prod = "?"
        out[(d.bus, d.address)] = (d.idProduct, d.bcdDevice, prod)
    return out


def fmt(key, info):
    return f"bus {key[0]} addr {key[1]} PID {info[0]:04X} bcd {info[1]:04X} \"{info[2]}\""


def main():
    rotulo = sys.argv[1] if len(sys.argv) > 1 else "teste"
    os.makedirs("port/logs", exist_ok=True)
    log_path = f"port/logs/reset_{rotulo}.txt"
    mon_path = f"port/logs/usbmon_{rotulo}.txt"
    log = open(log_path, "w", buffering=1)

    def say(msg):
        line = f"[{time.strftime('%H:%M:%S')}] {msg}"
        print(line)
        log.write(line + "\n")

    antes = listar()
    if not antes:
        say("Nenhum 8BitDo no barramento. Plugue o receptor (ou o cabo) com o controle ligado em DInput.")
        sys.exit(1)
    for k, v in antes.items():
        say("antes: " + fmt(k, v))
    if len(antes) > 1:
        say("Mais de um 8BitDo plugado; usando o primeiro.")
    alvo = sorted(antes)[0]
    bus = alvo[0]

    # usbmon: text interface of the bus, captured in the background
    subprocess.run(["modprobe", "usbmon"], check=False)
    if not os.path.ismount("/sys/kernel/debug"):
        subprocess.run(["mount", "-t", "debugfs", "none", "/sys/kernel/debug"], check=False)
    mon_file = f"/sys/kernel/debug/usb/usbmon/{bus}u"
    parar = threading.Event()

    def captura():
        try:
            with open(mon_file) as src, open(mon_path, "w", buffering=1) as dst:
                while not parar.is_set():
                    line = src.readline()
                    if line:
                        dst.write(line)
        except OSError as e:
            say(f"usbmon indisponivel ({e}); sigo sem ele")

    t = threading.Thread(target=captura, daemon=True)
    t.start()
    time.sleep(1.0)

    dev = usb.core.find(idVendor=VID, bus=alvo[0], address=alvo[1])
    say(f"resetando {fmt(alvo, antes[alvo])} (o Linux vai reler os descritores e configurar sem pedir strings)")
    try:
        dev.reset()
        say("reset concluido")
    except usb.core.USBError as e:
        say(f"reset devolveu erro: {e}")

    atual = listar()
    fim = time.time() + 20
    saidas = entradas = 0
    while time.time() < fim:
        time.sleep(0.2)
        agora = listar()
        for k in sorted(set(atual) - set(agora)):
            saidas += 1
            say("SAIU   " + fmt(k, atual[k]))
        for k in sorted(set(agora) - set(atual)):
            entradas += 1
            say("ENTROU " + fmt(k, agora[k]))
        atual = agora
    parar.set()

    for k, v in atual.items():
        say("depois: " + fmt(k, v))
    if saidas == 0:
        say("RESULTADO: continuou conectado depois de ser configurado sem strings.")
    else:
        say(f"RESULTADO: saiu do barramento {saidas} vez(es) e entrou {entradas} vez(es) depois do reset.")

    kern = subprocess.run(["journalctl", "-k", "--since", "-1min", "--no-pager"], capture_output=True, text=True).stdout
    log.write("\n--- log do kernel (ultimo minuto) ---\n")
    log.write("\n".join(l for l in kern.splitlines() if "usb" in l.lower() or "hid" in l.lower() or "8bitdo" in l.lower()) + "\n")
    log.close()
    uid, gid = int(os.environ.get("SUDO_UID", os.getuid())), int(os.environ.get("SUDO_GID", os.getgid()))
    for p in (log_path, mon_path):
        if os.path.exists(p):
            os.chown(p, uid, gid)
    print(f"\nPronto: {log_path} e {mon_path}")


if __name__ == "__main__":
    main()
