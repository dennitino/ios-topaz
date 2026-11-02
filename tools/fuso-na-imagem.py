#!/usr/bin/env python3
"""Troca o fuso horário do iOS direto no arquivo ios16-root.simg, sem abrir o disco (serve no WSL).

O fuso do iOS é um atalho (symlink) /var/db/timezone/localtime, cujo conteúdo é o texto
    /var/db/timezone/zoneinfo/America/Los_Angeles
Esse texto fica guardado como está dentro da imagem. Este script troca ele, no mesmo lugar e com
o mesmo tamanho, pelo fuso escolhido; o que sobra vira bytes zero, onde a leitura do caminho
termina (o kernel e o CoreFoundation param no primeiro zero).

Muda só esses 45 bytes do arquivo, e guarda o que estava lá em ARQUIVO.fuso-desfazer para voltar
atrás com --desfazer.

Uso (Ubuntu do WSL, na pasta da imagem):
    python3 fuso-na-imagem.py ios16-root.simg America/Bahia
    python3 fuso-na-imagem.py ios16-root.simg --desfazer
Ler os 12 GB demora (uns 10 a 20 minutos pelo WSL); a porcentagem aparece na tela.
"""
import os
import sys

PREFIX = b"/var/db/timezone/zoneinfo/"
OLD = PREFIX + b"America/Los_Angeles"
CHUNK = 64 << 20


def find_all(path, needle):
    size = os.path.getsize(path)
    found, done, tail = [], 0, b""
    with open(path, "rb") as f:
        while True:
            block = f.read(CHUNK)
            if not block:
                break
            data = tail + block
            base = done - len(tail)
            at = data.find(needle)
            while at != -1:
                if base + at not in found:
                    found.append(base + at)
                at = data.find(needle, at + 1)
            tail = data[-(len(needle) - 1):]
            done += len(block)
            print(f"\r  lendo: {done * 100 // size:3d}%", end="", flush=True)
    print()
    return found


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    path, what = sys.argv[1], sys.argv[2]
    undo = path + ".fuso-desfazer"

    if what == "--desfazer":
        if not os.path.exists(undo):
            sys.exit("ERRO: não há nada para desfazer nesta imagem.")
        with open(undo, "rb") as u:
            offset = int(u.readline())
            original = u.read()
        with open(path, "r+b") as f:
            f.seek(offset)
            f.write(original)
        os.remove(undo)
        print(f"Desfeito: o fuso voltou ao original ({original.decode()}).")
        return

    zone = what.encode()
    new = PREFIX + zone
    if len(new) > len(OLD):
        sys.exit(f"ERRO: o nome do fuso pode ter no máximo {len(OLD) - len(PREFIX)} caracteres.")
    if os.path.exists(undo):
        sys.exit("ERRO: esta imagem já foi alterada; use --desfazer primeiro.")

    print(f"Procurando o fuso atual em {path}")
    places = find_all(path, OLD)
    if len(places) != 1:
        sys.exit(f"ERRO: esperava achar o fuso 1 vez, achei {len(places)}. Nada foi mudado.")
    offset = places[0]
    with open(path, "r+b") as f:
        f.seek(offset)
        if f.read(len(OLD)) != OLD:
            sys.exit("ERRO: o arquivo mudou durante a leitura. Nada foi mudado.")
        with open(undo, "wb") as u:
            u.write(f"{offset}\n".encode() + OLD)
        f.seek(offset)
        f.write(new + b"\0" * (len(OLD) - len(new)))
    print(f"Pronto: fuso trocado para {what} (posição {offset} do arquivo).")
    print(f"Para voltar atrás: python3 {os.path.basename(sys.argv[0])} {path} --desfazer")


if __name__ == "__main__":
    main()
