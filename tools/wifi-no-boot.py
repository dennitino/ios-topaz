#!/usr/bin/env python3
"""Coloca o nome e a senha do Wi-Fi numa imagem de boot do ios-topaz (patch 0952).

A imagem é gerada com espaços reservados de tamanho fixo nas opções de boot:
    wifi_ssid=################################ wifi_psk=###...### (63 '#')
Este script troca esses espaços pelo nome e pela senha e salva uma cópia nova.
A senha fica só no seu computador, dentro do arquivo gerado.

Uso (Windows, Prompt de Comando):
    python wifi-no-boot.py shim-ios-wifi-modelo.img shim-ios-wifi-casa.img "NOME_DA_REDE" "SENHA"
"""
import sys

SSID_SLOT = b"wifi_ssid=" + b"#" * 32
PSK_SLOT = b"wifi_psk=" + b"#" * 63


def fill(slot, key, value):
    if b" " in value:
        sys.exit(f"ERRO: {key.decode()} não pode ter espaços (as opções de boot se separam por espaço).")
    new = key + b"=" + value
    if len(new) > len(slot):
        sys.exit(f"ERRO: {key.decode()} longo demais.")
    return new + b" " * (len(slot) - len(new))


def main():
    if len(sys.argv) != 5:
        print(__doc__)
        sys.exit(2)
    src, dst = sys.argv[1], sys.argv[2]
    ssid = sys.argv[3].encode("utf-8")
    psk = sys.argv[4].encode("utf-8")
    if not 1 <= len(ssid) <= 32:
        sys.exit("ERRO: o nome da rede precisa ter de 1 a 32 caracteres.")
    if psk and not 8 <= len(psk) <= 63:
        sys.exit("ERRO: a senha WPA2 precisa ter de 8 a 63 caracteres.")
    data = open(src, "rb").read()
    for slot in (SSID_SLOT, PSK_SLOT):
        if data.count(slot) != 1:
            sys.exit("ERRO: esta imagem não tem os espaços reservados do Wi-Fi (use o modelo certo).")
    data = data.replace(SSID_SLOT, fill(SSID_SLOT, b"wifi_ssid", ssid))
    data = data.replace(PSK_SLOT, fill(PSK_SLOT, b"wifi_psk", psk) if psk else b"wifi_psk=#" + b" " * (len(PSK_SLOT) - 10))
    open(dst, "wb").write(data)
    print(f"Pronto: {dst} (rede {ssid.decode('utf-8')})")


if __name__ == "__main__":
    main()
