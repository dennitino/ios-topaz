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
