#!/usr/bin/env python3
"""Descobre o layout dos reports HID de um controle DirectInput, para a tabela kRawLayouts.

Lê todas as interfaces HID do controle ao mesmo tempo (/dev/hidrawN), grava o report
descriptor de cada uma e, para cada botão e analógico, compara os reports com o repouso e
aponta qual byte (e qual bit) muda. Tudo vai para port/logs/mapa_<nome>.json e um resumo
legível em port/logs/mapa_<nome>.txt.

Uso: sudo port/.venv/bin/python -u tools/mapear_controle.py <nome> <VID> <PID>
  ex.: sudo port/.venv/bin/python -u tools/mapear_controle.py easysmx 2345 E037
"""
import glob
import json
import os
import select
import sys
import time

CONTROLES = [
    ("A", "botao"), ("B", "botao"), ("X", "botao"), ("Y", "botao"),
    ("LB", "botao"), ("RB", "botao"), ("LT", "gatilho"), ("RT", "gatilho"),
    ("BACK", "botao"), ("START", "botao"), ("L3", "botao"), ("R3", "botao"), ("HOME", "botao"),
    ("DPAD_CIMA", "dpad"), ("DPAD_BAIXO", "dpad"), ("DPAD_ESQUERDA", "dpad"), ("DPAD_DIREITA", "dpad"),
    ("ANALOGICO_ESQ_ESQUERDA", "eixo"), ("ANALOGICO_ESQ_DIREITA", "eixo"),
    ("ANALOGICO_ESQ_CIMA", "eixo"), ("ANALOGICO_ESQ_BAIXO", "eixo"),
    ("ANALOGICO_DIR_ESQUERDA", "eixo"), ("ANALOGICO_DIR_DIREITA", "eixo"),
    ("ANALOGICO_DIR_CIMA", "eixo"), ("ANALOGICO_DIR_BAIXO", "eixo"),
]


def find_nodes(vid, pid):
    """hidraw nodes of the device, with their USB interface number."""
    want = f"0003:{vid:08X}:{pid:08X}"
    nodes = []
    for path in sorted(glob.glob("/sys/class/hidraw/hidraw*")):
        try:
            uevent = open(os.path.join(path, "device", "uevent")).read()
        except OSError:
            continue
        if f"HID_ID={want}" not in uevent.upper():
            continue
        real = os.path.realpath(os.path.join(path, "device"))
        # .../1-2:1.0/0003:2345:E037.0007 -> interface 0
        iface = next((p.split(".")[-1] for p in real.split("/") if ":" in p and "." in p and p.count(":") == 1), "?")
        desc = open(os.path.join(path, "device", "report_descriptor"), "rb").read()
        name = next((l.split("=", 1)[1] for l in uevent.splitlines() if l.startswith("HID_NAME=")), "")
        nodes.append({"node": "/dev/" + os.path.basename(path), "interface": iface, "name": name, "descriptor": desc.hex()})
    return nodes


def record(fds, seconds):
    """All reports received in 'seconds', per node."""
    out = {node: [] for node in fds}
    end = time.time() + seconds
    while time.time() < end:
        ready, _, _ = select.select(list(fds.values()), [], [], 0.05)
        for node, fd in fds.items():
            if fd in ready:
                try:
                    out[node].append(os.read(fd, 256).hex())
                except BlockingIOError:
                    pass
                except OSError:
                    raise Reconnected()
    return out


class Reconnected(Exception):
    """The device went away (e.g. the 8BitDo receiver leaving its IDLE phase)."""


def wait_ready(vid, pid):
    """Nodes of the device once it is present, not in an IDLE phase, and stable for 2 s."""
    print(f"Procurando {vid:04X}:{pid:04X}... (plugue, ligue o controle e, se for o caso, troque para DInput)")
    shown = None
    stable_since = None
    deadline = time.time() + 120
    while time.time() < deadline:
        nodes = find_nodes(vid, pid)
        key = [(n["node"], n["name"]) for n in nodes]
        if nodes and any("IDLE" in n["name"].upper() for n in nodes):
            if shown != key:
                print(f"  {nodes[0]['name']}: fase IDLE, esperando o controle se conectar ao receptor...")
            stable_since = None
        elif nodes:
            if key != shown:
                stable_since = time.time()
            elif stable_since and time.time() - stable_since >= 2:
                return nodes
        shown = key
        time.sleep(0.3)
    print("Controle nao ficou pronto.")
    sys.exit(1)


def open_nodes(nodes):
    for n in nodes:
        print(f"  {n['node']}: interface {n['interface']}, {n['name']}, descritor de {len(n['descriptor']) // 2} bytes")
    return {n["node"]: os.open(n["node"], os.O_RDONLY | os.O_NONBLOCK) for n in nodes}


def most_common(reports):
    if not reports:
        return None
    counts = {}
    for r in reports:
        counts[r] = counts.get(r, 0) + 1
    return max(counts, key=counts.get)


def diff(base_hex, reports):
    """Bytes that differ from the rest report, with the bits that changed and value range."""
    if base_hex is None or not reports:
        return []
    base = bytes.fromhex(base_hex)
    result = {}
    for r in reports:
        b = bytes.fromhex(r)
        for i in range(min(len(base), len(b))):
            if b[i] != base[i]:
                e = result.setdefault(i, {"repouso": base[i], "valores": set(), "bits": 0})
                e["valores"].add(b[i])
                e["bits"] |= b[i] ^ base[i]
    return [{"byte": i, "repouso": f"{e['repouso']:02X}", "bits_mudaram": f"{e['bits']:08b}",
             "min": f"{min(e['valores']):02X}", "max": f"{max(e['valores']):02X}"} for i, e in sorted(result.items())]


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(1)
    nome, vid, pid = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16)
    nodes = wait_ready(vid, pid)
    fds = open_nodes(nodes)
    resultado = {"nome": nome, "vid": f"{vid:04X}", "pid": f"{pid:04X}", "interfaces": nodes, "controles": {}}

    def reopen():
        nonlocal nodes, fds
        print("  o controle se reconectou; reabrindo...")
        for fd in fds.values():
            try:
                os.close(fd)
            except OSError:
                pass
        nodes = wait_ready(vid, pid)
        fds = open_nodes(nodes)
        resultado["interfaces"] = nodes

    while True:
        input("\nSolte tudo e aperte ENTER. Nao toque no controle por 3 segundos...")
        try:
            repouso = record(fds, 3)
            break
        except Reconnected:
            reopen()
    base = {node: most_common(r) for node, r in repouso.items()}
    resultado["repouso"] = {node: {"report": base[node], "quantidade": len(r)} for node, r in repouso.items()}
    for node in fds:
        print(f"  {node}: {len(repouso[node])} reports em repouso, tipico {base[node]}")

    for controle, tipo in CONTROLES:
        acao = "mova o analogico ate o fim e SEGURE" if tipo == "eixo" else "SEGURE (aperte ate o fundo)"
        while True:
            input(f"\n[{controle}] Aperte ENTER e, em seguida, {acao} por 3 segundos...")
            time.sleep(0.3)
            try:
                capt = record(fds, 3)
                break
            except Reconnected:
                reopen()
                print("  (repita este controle)")
        resultado["controles"][controle] = {}
        for node in fds:
            mud = diff(base.get(node), capt[node])
            resultado["controles"][controle][node] = {"reports": len(capt[node]), "mudancas": mud}
            if mud:
                print(f"  {node}: " + ", ".join(f"byte {m['byte']} bits {m['bits_mudaram']} {m['repouso']}->{m['min']}..{m['max']}" for m in mud))
        if not any(resultado["controles"][controle][n]["mudancas"] for n in fds):
            print("  (nada mudou: tente de novo depois, ou esse controle nao existe)")

    os.makedirs("port/logs", exist_ok=True)
    with open(f"port/logs/mapa_{nome}.json", "w") as f:
        json.dump(resultado, f, indent=1)
    with open(f"port/logs/mapa_{nome}.txt", "w") as f:
        for n in nodes:
            f.write(f"{n['node']} interface {n['interface']} descritor {n['descriptor']}\n")
        for node in fds:
            f.write(f"repouso {node}: {base[node]}\n")
        for controle, por_node in resultado["controles"].items():
            for node, info in por_node.items():
                for m in info["mudancas"]:
                    f.write(f"{controle:24} {node} byte {m['byte']:2} bits {m['bits_mudaram']} {m['repouso']}->{m['min']}..{m['max']}\n")
    print(f"\nPronto: port/logs/mapa_{nome}.json e port/logs/mapa_{nome}.txt")
    for fd in fds.values():
        os.close(fd)
    os.chown(f"port/logs/mapa_{nome}.json", int(os.environ.get("SUDO_UID", os.getuid())), int(os.environ.get("SUDO_GID", os.getgid())))
    os.chown(f"port/logs/mapa_{nome}.txt", int(os.environ.get("SUDO_UID", os.getuid())), int(os.environ.get("SUDO_GID", os.getgid())))


if __name__ == "__main__":
    main()
