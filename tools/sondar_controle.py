#!/usr/bin/env python3
"""Sonda um controle no PC do jeito que o kernel 17150 do Xbox 360 faz antes de escolher o driver.

1. Espera um dispositivo USB novo ser plugado e mostra classe e interfaces.
2. Pede a string 0xEE ("MSFT100", o descritor de SO da Microsoft). O kernel 17150 so segue
   para a escolha de driver se a resposta for um STALL limpo ou um MSFT100 valido; qualquer
   outro erro descarta o dispositivo (0x800D8184-0x800D8198 no dump).
3. Se vier MSFT100, pede o Extended Compat ID (wIndex 4), como o kernel faz.
4. Fica 90 s observando o USB. Cada dispositivo que entrar nesse tempo (por exemplo o
   EasySMX depois de trocar de XInput para DInput) tambem e descrito e sondado.

Uso: sudo port/.venv/bin/python tools/sondar_controle.py
"""
import time
import usb.core
import usb.util


def snapshot():
    return {(d.idVendor, d.idProduct, d.bus, d.address) for d in usb.core.find(find_all=True)}


def fmt(key):
    return f"{key[0]:04X}:{key[1]:04X} (bus {key[2]} addr {key[3]})"


def describe(dev):
    print(f"  device class {dev.bDeviceClass:02X}/{dev.bDeviceSubClass:02X}/{dev.bDeviceProtocol:02X}, "
          f"bcdUSB {dev.bcdUSB:04X}, maxpacket0 {dev.bMaxPacketSize0}")
    for attr in ("manufacturer", "product"):
        try:
            print(f"  {attr}: {getattr(dev, attr)}")
        except Exception as e:
            print(f"  {attr}: (erro: {e})")
    for cfg in dev:
        for intf in cfg:
            eps = ", ".join(f"EP {ep.bEndpointAddress:02X} max {ep.wMaxPacketSize}" for ep in intf)
            print(f"  interface {intf.bInterfaceNumber} alt {intf.bAlternateSetting}: classe "
                  f"{intf.bInterfaceClass:02X}/{intf.bInterfaceSubClass:02X}/{intf.bInterfaceProtocol:02X}  [{eps}]")


def probe_ms_os(dev):
    for langid in (0x0000, 0x0409):
        try:
            data = bytes(dev.ctrl_transfer(0x80, 6, 0x03EE, langid, 0x12, timeout=1000))
        except usb.core.USBError as e:
            kind = "STALL (ok para o Xbox)" if e.errno == 32 else ("TIMEOUT (Xbox descarta)" if e.errno == 110 else "ERRO (Xbox descarta)")
            print(f"  string 0xEE langid {langid:04X}: {kind} -> {e}")
            continue
        print(f"  string 0xEE langid {langid:04X}: {len(data)} bytes {data.hex(' ')}")
        if len(data) >= 18 and data[2:16] == "MSFT100".encode("utf-16-le"):
            vendor = data[16]
            print(f"  MSFT100 valido, vendor code {vendor:02X}; pedindo Extended Compat ID")
            try:
                compat = bytes(dev.ctrl_transfer(0xC0, vendor, 0x0000, 0x0004, 0x28, timeout=1000))
                print(f"  compat ID: {compat.hex(' ')}")
            except usb.core.USBError as e:
                print(f"  compat ID: erro {e}")
        else:
            print("  resposta NAO e MSFT100 (Xbox descarta)")


def main():
    before = snapshot()
    print("Plugue o controle agora (espero 60 s)...")
    new = set()
    deadline = time.time() + 60
    while time.time() < deadline and not new:
        time.sleep(0.3)
        new = snapshot() - before
    if not new:
        print("Nenhum dispositivo novo apareceu.")
        return
    time.sleep(1.0)  # deixa o Linux terminar a enumeracao
    for key in sorted(new):
        dev = usb.core.find(idVendor=key[0], idProduct=key[1], bus=key[2], address=key[3])
        print(f"\nNovo dispositivo: {fmt(key)}")
        if dev is None:
            print("  sumiu antes de ser lido (ja se reconectou?)")
            continue
        describe(dev)
        probe_ms_os(dev)

    print("\nAgora troque de modo (se for o caso), aperte botoes e mexa os analogicos por 90 s;")
    print("mostro qualquer reconexao e sondo cada dispositivo novo.")
    current = snapshot()
    end = time.time() + 90
    while time.time() < end:
        time.sleep(0.3)
        now = snapshot()
        for key in sorted(current - now):
            print(f"  [{time.strftime('%H:%M:%S')}] SAIU   {fmt(key)}")
        for key in sorted(now - current):
            print(f"  [{time.strftime('%H:%M:%S')}] ENTROU {fmt(key)}")
            time.sleep(1.0)
            dev = usb.core.find(idVendor=key[0], idProduct=key[1], bus=key[2], address=key[3])
            if dev is not None:
                describe(dev)
                probe_ms_os(dev)
        current = snapshot() if now - current else now
    print("Fim.")


if __name__ == "__main__":
    main()
