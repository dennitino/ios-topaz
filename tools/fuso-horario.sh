#!/bin/bash
# fuso-horario.sh — troca o fuso horário do iOS gravado numa imagem ios16-root.simg.
#
# O menu Ajustes → Geral → Data e Hora não abre neste porte, e o iOS fica no fuso da
# Califórnia (America/Los_Angeles). Este script abre a imagem do sistema, aponta o fuso para o
# escolhido e gera uma imagem nova, para gravar com fastboot (apaga o que estiver no iOS).
#
# Precisa de um Linux com suporte a HFS+ (o Ubuntu do pendrive serve; o WSL não tem).
#
# Uso:
#   bash fuso-horario.sh ENTRADA.simg SAIDA.simg [FUSO]
# Exemplo:
#   bash fuso-horario.sh ios16-root.simg ios16-root-bahia.simg America/Bahia
#
# Precisa de ~30 GB livres na pasta da SAIDA (a imagem aberta tem 16 GB).
set -euo pipefail

[[ $# -ge 2 ]] || { sed -n '2,15p' "$0"; exit 2; }
IN=$(realpath "$1")
OUT=$(realpath -m "$2")
TZNAME=${3:-America/Bahia}
RAW="${OUT%.simg}.img"
MNT=/mnt/iosroot-fuso

msg()  { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
erro() { printf '\n\033[1;31mERRO: %s\033[0m\n' "$*" >&2; exit 1; }
sair() { mountpoint -q "$MNT" && sudo umount "$MNT" || true; }
trap sair EXIT

[[ -f $IN ]] || erro "não achei $IN"
[[ $IN != "$OUT" ]] || erro "a SAIDA precisa ter outro nome"

msg "1/6 Pacotes"
sudo apt-get update -q >/dev/null || true
sudo apt-get install -y -q android-sdk-libsparse-utils hfsprogs >/dev/null
sudo modprobe hfsplus || true
grep -qw hfsplus /proc/filesystems || erro "este Linux não abre HFS+ (o WSL não abre: use o Ubuntu do pendrive)"

msg "2/6 Abrindo a imagem (16 GB, demora)"
simg2img "$IN" "$RAW"

msg "3/6 Montando"
fsck.hfsplus -f -y "$RAW" >/dev/null || true
sudo mkdir -p "$MNT"
sudo mount -t hfsplus -o loop,force "$RAW" "$MNT"

msg "4/6 Trocando o fuso para $TZNAME"
# os arquivos de fuso que existem nesta imagem
zi=""
for d in usr/share/zoneinfo.default usr/share/zoneinfo; do
	[[ -f $MNT/$d/$TZNAME ]] && { zi=/$d; break; }
done
[[ -n $zi ]] || erro "o fuso $TZNAME não existe nesta imagem (veja os nomes em $MNT/usr/share/zoneinfo*)"
echo "arquivos de fuso: $zi"

# o que existe hoje
for l in private/var/db/timezone/localtime private/etc/localtime; do
	if [[ -L $MNT/$l ]]; then
		echo "antes: /$l -> $(readlink "$MNT/$l")"
	else
		echo "antes: /$l não existe"
	fi
done

# /var/db/timezone/localtime é o que o iOS lê (e o que Data e Hora troca)
sudo mkdir -p "$MNT/private/var/db/timezone"
sudo ln -sfn "$zi/$TZNAME" "$MNT/private/var/db/timezone/localtime"
# /etc/localtime aponta para ele (como no macOS), se ainda não existir outro
if [[ ! -e $MNT/private/etc/localtime || -L $MNT/private/etc/localtime ]]; then
	sudo ln -sfn /private/var/db/timezone/localtime "$MNT/private/etc/localtime"
fi
echo "depois: /private/var/db/timezone/localtime -> $(readlink "$MNT/private/var/db/timezone/localtime")"
echo "depois: /private/etc/localtime -> $(readlink "$MNT/private/etc/localtime" || echo '(arquivo, não mexido)')"

# a preferência do iOS: fuso escolhido à mão (não pela localização, que aqui não existe)
python3 - "$MNT" "$TZNAME" <<'EOF'
import os, plistlib, sys
root, tz = sys.argv[1], sys.argv[2]
path = os.path.join(root, 'private/var/mobile/Library/Preferences/com.apple.timezone.auto.plist')
data = {}
if os.path.exists(path):
    with open(path, 'rb') as f:
        data = plistlib.load(f)
data['Active'] = False
os.makedirs(os.path.dirname(path), exist_ok=True)
with open(path, 'wb') as f:
    plistlib.dump(data, f, fmt=plistlib.FMT_BINARY)
os.chown(path, 501, 501)
os.chmod(path, 0o600)
print('preferência: ajuste automático do fuso desligado')
EOF

msg "5/6 Fechando a imagem"
sudo umount "$MNT"
fsck.hfsplus -f -y "$RAW" >/dev/null || true
fsck.hfsplus -f -n "$RAW" || echo "aviso: fsck pediu reparo menor (inofensivo)"

msg "6/6 Gerando $OUT"
img2simg "$RAW" "$OUT"
rm -f "$RAW"
ls -lh "$OUT"
echo
echo "Pronto. Grave com:  fastboot -S 256M flash userdata \"$(basename "$OUT")\""
echo "e dê o primeiro boot já com a imagem de Wi-Fi (shim-ios-wifi5-casa.img)."
