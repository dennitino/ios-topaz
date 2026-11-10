# ios-topaz — branch `denis`

Fork de [MakrSas/ios-topaz](https://github.com/MakrSas/ios-topaz) para o Redmi Note 12 4G do Denis.
A branch `topaz` acompanha o projeto original; as nossas mudanças ficam aqui, na `denis`.

## Aparelho

| Item | Valor |
| --- | --- |
| Modelo | Redmi Note 12 4G (`topaz`), Snapdragon 685, 8 GB de RAM |
| Android antes do iOS | HyperOS 2.0.209 Global (`VMTMIXM`) |
| Tela | **Paralela TFT LCD** (4STORE PREMIUM, modelo TXR-N12), no lugar da AMOLED original. O ABL a identifica como `m7 38 0c 0a` |
| Chip de toque | Responde como FocalTech FT5452 (chip-id `54:52`), vendor `0x48`, **firmware `0x32`** (vem com a tela paralela) |
| iOS gravado | iOS 16.7.16 (20H392) do iPhone X, partição `userdata` |

## Situação (29/10/2026)

- **Funciona:** boot até a tela inicial, **toque** (com o patch 0951), relógio, botões de volume
  e power, Siri.

### Solução do toque (29/10/2026)

O chip da tela paralela devolve só zeros quando a leitura começa no registrador **0x00** e
devolve os toques quando começa no **0x01**, que é como o driver do Android lê. Descoberto com o
laboratório da shim (`shim/touchlab.c`, `SHIM_CFLAGS=-DTOUCHLAB`). Correção:
`xnu/patches/0951-topaz-touch-read-from-reg-01.patch`. Testado no aparelho com o kernel
`e477291` alterado direto no binário (3 instruções em `_touch_thread`), depois com o kernel
compilado da branch `denis` (run 41002427011, 30/10/2026): toque funcionando nos dois.

### Diagnóstico do toque

Kernel `e477291` com `sweettouchdiag=2`, tocando a tela o tempo todo:

```
SWEET TOUCH: TLMM reset=0 irq=0x1 avdd=0
SWEET TOUCH: probe 0x38 err=0/0 chip-id=54:52
SWEET TOUCH: ft fw=32/0 vendor=48/0 devmode=00/0 pmode=00/0 lib=0000/0
SWEET TOUCH stats: frames=315 errs=0 lasterr=0 maxn=0 hid down=0 up=0 last=0,0 | raw 00 00 00 | 00 00 00 00 00 00 | 00 00 00
```

- O I2C funciona e o chip responde, mas **todo relatório vem zerado** (`TD_STATUS` sempre 0)
  e a IRQ nunca dispara.
- O aparelho do desenvolvedor tem o mesmo chip com **firmware `0x36`**
  (topaz-woa, `docs/AGENT_BRIEF_drivers.md`, seção 2b), e nele o toque funciona.

### Já testado, sem efeito

- Correção 0950 (relatório com 0 dedos solta todos), aplicada direto no kernel antigo `2665612`.
- Segurar o reset por 500 ms depois do AVDD e esperar 1 s depois do reset
  (`touch_wait(20)` → 500 e `touch_wait(250)` → 1000), no kernel `e477291`.

### Causa provável (29/10/2026)

A tela foi trocada por uma **paralela TFT LCD**. O chip de toque fica no conjunto da tela, então
o firmware `0x32` é o da peça paralela, não o da tela AMOLED original (`0x36`). Numa tela LCD desse
tipo o toque costuma depender do sincronismo com a imagem; o driver do Android funciona com ela,
o do ios-topaz (testado só com a tela original) não. Pinos (IRQ 80, reset 86, AVDD 36) e tela
reportada (`m7 38 0c 0a`) são os mesmos da device tree oficial.

### Próximos passos

1. Feito: o driver do Android (`focaltech_ts_i2c.ko`, IC "ft3519t", vendor "samsung") só faz reset,
   espera 200 ms e lê a partir do registrador 0x01; nenhum comando extra. Testar `sweetfb_hz=60`.
   Caminho garantido: tela AMOLED original.
2. Perguntado ao desenvolvedor (MakrSas) se o firmware `0x32` precisa de algum passo extra.

## Como gerar e dar boot

- **Kernel:** Actions → `xnu` → Run workflow (branch `denis` ou `topaz`).
  Precisa do runner `macos-14` (Xcode 15), que o GitHub desliga em 2/11/2026.
- **Imagem do iOS e boot:** `tools/preparar-ios-topaz.sh` (Ubuntu), com ajustes para o
  Ubuntu 26.04 (CMake 4, GCC 15, `install` do uutils) e caminhos com espaço.
- **Boot pelo Windows:**
  ```
  fastboot boot shim-ios.img
  ```
  O boot é só na memória: a cada religada, precisa do PC.

## Wi-Fi (30/10/2026)

- Com o patch 0952 (`wifi_ssid=`/`wifi_psk=` nas opções de boot) o kernel entra na rede sozinho:
  scan, autenticação, associação, handshake WPA2 (mensagem 1 → 2 enviada) e `net: the link (wcn3988) is up`.
  Senha certa (com senha errada o kernel diz "wrong passphrase").
- Mesmo assim o iOS não usa a conexão: o `wifid` só lê SSID/BSSID, o `configd` não mexe no `en0`
  depois do link e nenhum quadro sai (0 enviados), ou seja, não há DHCP.
- Hipótese: o `configd` só configura IP quando a entrada na rede foi pedida pelo próprio `wifid`
  (Ajustes). A tela de senha não abre aqui, então o próximo teste é uma **rede aberta** (sem senha),
  que o `wifid` pede sem abrir a tela de senha.

### Wi-Fi conectado (02/11/2026)

Com os patches 0953 (rede do boot mostrada como aberta ao wifid), 0955 (IOKit ocupado até o en0
existir) e 0956 (controlador como IONetworkController, igual ao HackiOS atual), kernel da run
41446579169, iOS regravado e primeiro boot já com a imagem de Wi-Fi: Ajustes mostra o ✓ na rede
e o ícone do Wi-Fi na barra de status. **Internet funcionando:** o Safari abre o Google e faz buscas.

## Atualizações dos projetos (10/11/2026)

- **ios-topaz original (MakrSas):** a branch `topaz` continua em `e477291` (22/10). O kernel dela, que é a
  base do nosso, já traz desligados: GPU (0901–0929, `gpu=1`), kernel de páginas 4K + AMP para os 8 núcleos
  (0140–0142), som (0144–0146, `adsp=1`), carregador (0148) e injeção de bibliotecas (0919, `topaz_inject=`).
  Falta a parte de usuário da GPU (Mesa + EAGL), que o projeto compila num Mac (`userland/mesa/mk-mesa-mac.sh`).
  Com a GPU o autor mediu 25–35 quadros por segundo nas animações.
- **HakiOS 2.3.4 (5/11):** só no POCO X5 Pro 5G. Metal na Adreno 642L (100–118 quadros por segundo), 8 núcleos,
  som e microfone, câmeras, vibração, `/private/var` mantido entre boots, SSH e Sileo; celular parcial; sem
  Bluetooth, sem carga sob esse kernel e sem serviços da Apple. O motor Metal recusa outras GPUs pelo nome.
- **Fork `NekomiyaRion-nya/ios-topaz`, branch `spes`** (Redmi Note 11, mesmo SM6225; último commit em 23/10):
  - `userland/mesa/mk-mesa-linux.sh` e `userland/gpu/build-linux.sh`: a parte de usuário da GPU compilada no
    Linux (clang-17, ld64.lld, SDK iPhoneOS 16.5 do theos), sem Mac;
  - patch `0965-spes-boot-files` + `mkpayload.py --files`: a imagem de boot leva arquivos e o kernel os instala
    na raiz do iOS antes do launchd (só os que faltam ou mudaram), sem regravar a `userdata`;
  - USB no SM6225 (0148–0150, 0969) para log ao vivo pelo cabo.
- **Fuso horário (pista do HakiOS, `userland/build.sh`):** os atalhos do modelo formam um círculo
  (`/usr/share/zoneinfo` aponta de volta para `/var`) até o daemon de fuso rodar. Eles apontam
  `/private/var/db/timezone/zoneinfo` para `/usr/share/zoneinfo.default` e o `localtime` para o fuso desejado.
- **Brilho (`sweetbl=1`):** sem GPU o CoreBrightness aplica um filtro em cada quadro e a interface cai para
  menos de 1 quadro por segundo (comentário do patch 0110). Só vale testar depois da GPU.
