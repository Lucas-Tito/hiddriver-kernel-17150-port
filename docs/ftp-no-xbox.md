# FTP no Xbox 360 (FreeStyle Dash)

O servidor FTP da FreeStyle evita ir e vir com o pendrive: dá para subir builds para o HD,
trocar o `hiddriver_etapa.txt` e baixar o `hiddriver_log.txt` direto do PC. Tudo aqui foi
testado no console do projeto (Xbox 360 E, kernel 17150, FreeStyle 3).

## Ligar e conectar

- **Na FreeStyle:** Configurações → rede → "Enable FTP server". O IP aparece na mesma tela.
  No console do projeto foi `192.168.0.4`, por DHCP, então pode mudar.
- **Login:** usuário `xbox`, senha `xbox`. O campo de senha aparece vazio na tela de
  configuração, mas a senha existe: com senha vazia o servidor responde `530 Incorrect Password`.
- **O console precisa estar na FreeStyle.** Travado ou desligado, a placa de rede ainda responde
  na rede local (o MAC aparece no `ip neigh`), mas nenhum serviço responde.
- **Ping não serve de teste.** O console não respondeu ICMP. Teste a porta direto:
  `bash -c "</dev/tcp/192.168.0.4/21"`. As portas 80 e 9999 (HTTP da FreeStyle) também ficam
  abertas.

## O servidor (F3 FTPD) e as manias dele

| Comportamento | O que fazer |
|---|---|
| Atende **uma conexão por vez**; uma sessão que ficou pendurada bloqueia a próxima (timeout na saudação) | esperar uns 10 s e tentar de novo, em laço |
| **Não responde ao `QUIT`** | fechar o socket direto (`ftp.close()`, não `ftp.quit()`) |
| `RETR` com **caminho absoluto** dá `550 Invalid argument` | `CWD /Hdd1` e depois `RETR nome` (o `curl` com URL completa funciona porque faz o `CWD` sozinho) |
| **Sem retomada** (`REST`): `curl -C -` dá erro 31 | baixar o arquivo inteiro de uma vez |
| **Trava ao ler um arquivo que está sendo gravado** (o log do hiddriver, que recebe uma linha a cada 5 s) | renomear antes (`RNFR`/`RNTO` funcionam), baixar o renomeado; o plugin começa um log novo |
| `STOR` funciona, **inclusive por cima do `hiddriver.xex` carregado** | o arquivo novo vale a partir do próximo boot |

Raiz do servidor: `/Hdd1` (HD interno), `/Game` e `/OnBoardMU` (a memória interna do Xbox E).
O `launch.ini` do DashLaunch fica em `/Hdd1/launch.ini`.

## Receitas

Baixar o log sem travar o servidor (renomeia e baixa):

```python
import ftplib, time

def conectar():
    for _ in range(5):
        try:
            f = ftplib.FTP()
            f.connect("192.168.0.4", 21, timeout=25)
            f.login("xbox", "xbox")
            return f
        except Exception:
            time.sleep(10)  # sessão anterior ainda pendurada
    raise SystemExit("FTP não respondeu")

f = conectar()
f.cwd("/Hdd1")
f.sendcmd("RNFR hiddriver_log.txt")
f.sendcmd("RNTO hiddriver_log_teste.txt")
f.close()  # sem QUIT
```

```bash
curl -s --connect-timeout 20 --max-time 180 --user xbox:xbox \
     -o port/logs/teste.txt "ftp://192.168.0.4/Hdd1/hiddriver_log_teste.txt"
```

Subir um build e a etapa, e conferir pelo md5:

```python
import hashlib, io
f = conectar()
f.cwd("/Hdd1")
with open("build/bin/hiddriver.xex", "rb") as src:
    f.storbinary("STOR hiddriver.xex", src)
f.storbinary("STOR hiddriver_etapa.txt", io.BytesIO(b"2"))
buf = io.BytesIO()
f.retrbinary("RETR hiddriver.xex", buf.write)
print(hashlib.md5(buf.getvalue()).hexdigest())
f.close()
```

## Cuidados

- O FTP só escreve **arquivos do projeto** (o `hiddriver.xex`, o arquivo de etapa, os logs). Para
  mexer no `launch.ini`, baixe uma cópia antes.
- Uma sessão FTP pendurada pode deixar a FreeStyle lenta naquele boot. Reiniciar resolve.
