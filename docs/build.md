# Como compilar

O `hiddriver.xex` compila no Linux, dentro do container do XDK (a imagem e o XDK vêm do projeto
CollectionUI; ver o cabeçalho de `tools/build.sh`). Há dois builds do mesmo código, escolhidos por
uma chave de compilação.

| Comando | Saída | Para quê |
|---|---|---|
| `./tools/build.sh` | `build/uso/bin/hiddriver.xex` | **uso**: como o original, sem log nem notificação |
| `./tools/build.sh DIAG=1` | `build/diag/bin/hiddriver.xex` | **diagnóstico**: log no HD, batimento, notificação, arquivo de etapa |
| `./tools/build.sh STAGE=2` | `build/uso/bin/hiddriver.xex` | uso, sem reiniciar o USB (ver abaixo) |

Cada variante tem a sua pasta de objetos, então trocar de uma para a outra não mistura nada.

## O build de uso (`DIAG=0`, padrão)

Faz só o que os controles precisam, como o upstream:

- confere o kernel (17559, 17489 ou 17150) e não inicia com a bandeja do disco aberta;
- no 17150, confere byte a byte os endereços antes de qualquer patch (`check17150`);
- instala os ganchos do original (`HidAddDevice`, `HidRemoveDevice`, os três de input do xam e o de
  inatividade) e os três que carregam a correção do 8BitDo: conclusão do GET_DESCRIPTOR de
  configuração (strings antes do SET_CONFIGURATION), conclusão do SET_CONFIGURATION (SET_IDLE e
  report descriptor) e descarte de dispositivo (limpa o estado dessas sequências).

O `DbgPrint` vira nada: o binário não tem mensagens de log, nem apelidos de disco, nem a thread de
notificação, nem leitura do `hiddriver_etapa.txt`.

### Etapa fixa (`STAGE`)

- `STAGE=3` (padrão): reinicia o USB ao carregar, como o original, e pega controles já plugados no
  boot. Validada no 17150 em 02/10/2026 (`port/logs/teste_etapa3.txt`). Efeito colateral conhecido:
  depois do reinício, um dispositivo interno (provavelmente o Wi-Fi) fica sem driver; ver a issue #2.
- `STAGE=2`: só os ganchos, sem reiniciar o USB. **Os controles precisam ser plugados depois de o
  console ligar**, porque os que já estão plugados no boot são detectados antes de o plugin carregar.

## O build de diagnóstico (`DIAG=1`)

É o que foi usado em todos os testes. Grava `Hdd:\hiddriver_log.txt` (regras em
[`threads-e-contextos.md`](threads-e-contextos.md)), mostra a notificação "hiddriver: etapa N ativa",
lê a etapa e as letras de ganchos desligados de `Hdd:\hiddriver_etapa.txt` (sem o arquivo, etapa 1)
e instala os registros só de leitura da enumeração USB (escolha de driver, string `0xEE`, compat ID,
`UsbdAddDeviceComplete`).

## Subir para o console

Pelo FTP, ver [`ftp-no-xbox.md`](ftp-no-xbox.md). O arquivo vai para `/Hdd1/hiddriver.xex`, e o
`launch.ini` aponta para `Hdd:\hiddriver.xex`.
