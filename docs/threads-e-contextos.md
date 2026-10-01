# Threads e contextos de execução no hiddriver

O que foi aprendido portando a v0.5 para o kernel retail 17150, e as regras que valem para
qualquer código novo no plugin. Cada regra aponta o teste ou a evidência que a sustenta.

## Onde o código do plugin roda

| Contexto | Quem chama | O que pode fazer |
|---|---|---|
| `DllMain` | o loader, quando o DashLaunch carrega o plugin no boot | arquivo no HD, patches, instalar ganchos; é onde o upstream faz tudo |
| threads criadas pelo plugin (`MakeThread`) | nós: `EX_CREATE_FLAG_SYSTEM`, núcleo 4 | arquivo, `Sleep`, `XNotifyQueueUI` (com o patch abaixo), CRT |
| ganchos no **kernel** ligados ao USB (`HidAddDevice`, `HidRemoveDevice`, escolha de driver, `UsbdAddDeviceComplete`) | a enumeração USB, **com o spinlock do nó do dispositivo pego** | só trabalho curto: ler memória, contar, gravar números num buffer |
| ganchos no **xam** (`XamInputGetState`, `SetState`, `GetCapabilitiesEx`, `XamInactivityDetectRecentActivity`) | threads de título e de sistema, várias vezes por quadro | trabalho curto; nada que bloqueie o quadro |

## Regras

### 1. Nada da biblioteca C dentro dos ganchos do USB

`_vsnprintf`, `fopen`, `malloc` e parecidos não entram em gancho chamado pela enumeração USB.
Foi o `_vsnprintf` do nosso log que travou o console a cada dispositivo plugado.

O upstream não tinha o problema porque o `DbgPrint` dele é o do kernel. Ele só passou a existir
quando o `DbgPrint` foi redirecionado para o log em arquivo (`#define DbgPrint DiagLog`).

**Como o log faz agora:** o `DiagLog` guarda no anel só o ponteiro do formato e as palavras cruas
dos argumentos. Quem formata é a `FlushThread`, num contexto normal. Por isso, **todo argumento
`%s`/`%S` precisa apontar para dado estático**: literal ou variável global, nunca buffer local.

Bisseção que isolou a causa (etapa 2, pendrive plugado com o console ligado):

| Ganchos ligados (`hiddriver_etapa.txt`) | Resultado |
|---|---|
| nenhum (etapa `1`) | ok |
| só os do xam (`2am`) | ok |
| só `HidAddDevice`/`HidRemoveDevice` (`2mgsci`); o pendrive não é HID e não passa por eles | ok |
| todos (`2`), com os registros de USB (`m`) logando pelo `_vsnprintf` | **trava** |

O primeiro build da etapa 2, que não tinha os registros `m`, travava com o EasySMX, que é HID e
passa pelo `HidAddDevice`. Esse gancho também logava pelo `_vsnprintf`.

**Validado em 30/09/2026:** com a formatação adiada, a etapa `2` completa não trava nem com o
pendrive nem com o EasySMX plugados com o console ligado, e o log mostra o caminho de cada
dispositivo (`port/logs/teste_correcao.txt`):

| Dispositivo | Escolha de driver | Resultado |
|---|---|---|
| pendrive `058F:6387` | classe 08, mass storage | aceito |
| EasySMX em XInput `045E:028E` | classe FF/5D, XInput | rejeitado `C0051012` (autenticação; é o que o UsbdSecPatch remove) |
| EasySMX em DInput `2345:E037`, interfaces 0 e 1 | classe 03, `HidAddDevice` | passa pelo nosso gancho; fora da tabela, vai ao original e é rejeitado `C0000001` |

### 2. Não esperar dentro de gancho do USB

Uma espera limitada (100 ms, `GetTickCount` + `YieldProcessor`) dentro dos ganchos, para dar tempo
de o log ir ao disco, **gira segurando o spinlock do nó**. Se o gancho rodar no núcleo 4, o mesmo da
thread de gravação, ela nem roda. Isso muda o tempo exatamente onde o bug está e foi descartado
(revisão do crítico, `800D80BC`/`800D8168`/`800D7D1C` pegam o lock em `bl 0x800DA170`).

### 3. Arquivo a partir do plugin: links em `\System??\`

Código no processo de sistema resolve apelidos de drive em `\System??\`. O `\??\` é o namespace dos
títulos (apps), e um link criado ali é invisível para o `fopen` da thread do plugin. Esse erro
deixou dois builds sem log nenhum.

O `MountDiagDrives` cria `\System??\hidlogh:` para `\Device\Harddisk0\Partition1`. Se ele falhar,
usa o `hdd:` que o próprio DashLaunch já cria. Não conte com os apelidos da FreeStyle (`Hdd1:` é
nome dela, não do sistema).

### 4. Log só no HD, e aberto e fechado a cada linha

- **Só no HD:** o pendrive some durante o reinício do USB (etapa 3).
- **Abrir, gravar e fechar em cada linha:** um travamento não leva junto o que já foi gravado.
- **Batimento a cada 5 s** (`alive ... hits ...`). Ressalva: um contador incrementado
  imediatamente antes de um travamento **não aparece**, porque o batimento seguinte nunca sai.
  Zero no último batimento não prova que o gancho não rodou.

### 5. Notificação a partir do plugin

- O `XNotifyQueueUI` chamado do processo de sistema é descartado em silêncio, a não ser que o
  desvio do `xam` no ordinal 1183 + 48 vire incondicional (`0x409A` → `0x4800`). Esse é o commit
  `3049f04` do upstream, e o JRPC2 faz o mesmo. No 17150 a instrução é a mesma (`816A2480`).
- A chamada **retorna normalmente**: o log mostra "XNotifyQueueUI returned". Mesmo assim, ela roda
  na sua própria thread (`NotifyThread`), separada da de gravação.

### 6. Carregar DLL de sistema só de thread de sistema

`XexLoadImage` de um plugin de sistema chamado da thread principal de um título dá `0xC0000022`
(acesso negado). Os carregadores de plugin fazem isso de dentro de uma thread criada com
`EX_CREATE_FLAG_SYSTEM`.

### 7. Código escrito em tempo de execução

O `Detours` do iMoD1998 escrevia o salto com `memcpy`, sem sincronizar cache. Agora
`FlushCodeRange` (`Detours.cpp`) faz `dcbst; sync; icbi; isync` por linha de 128 bytes, a mesma
sequência que o kernel usa, provavelmente no `KeSweepIcacheRange` (ordinal 171). É boa prática e
não era a causa do travamento: as releituras depois do patch mostram o salto gravado
(`3C0081F1`).

## Testes no console

- **Dispositivo plugado no boot é enumerado antes dos ganchos.** Para trazer o log sem travar,
  ligue o console com o pendrive já plugado.
- **FTP da FreeStyle:** ver [`ftp-no-xbox.md`](ftp-no-xbox.md).
- **Formato do `.xex`:** `<dvdxgd2/>` no `xex.xml` liga o bit `0x8` das image flags ("só disco
  XGD2") e o console recusa o arquivo. O `tools/build.sh` barra isso.
