#!/bin/bash
# extrair-touch-android.sh — tira da ROM fastboot oficial do Redmi Note 12 4G (tapas) só o
# que importa para entender o toque: o driver FocalTech do Android (.ko), o firmware do chip
# e as linhas de configuração que os carregam. O resultado é um .tar.gz pequeno.
#
# Uso (Ubuntu):
#   bash extrair-touch-android.sh ROM.tgz PASTA_DE_TRABALHO
# Exemplo:
#   bash extrair-touch-android.sh ./tapas_global_images_OS2.0.209.0...tgz ./rom-trabalho
#
# A PASTA_DE_TRABALHO precisa de ~25 GB livres. Nada é gravado no celular.
set -euo pipefail

[[ $# -eq 2 ]] || { sed -n '2,11p' "$0"; exit 2; }
ROM=$(realpath "$1")
WORK=$(realpath -m "$2")
msg()  { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
erro() { printf '\n\033[1;31mERRO: %s\033[0m\n' "$*" >&2; exit 1; }
[[ -f $ROM ]] || erro "ROM não encontrada: $ROM"
mkdir -p "$WORK"
cd "$WORK"

msg "1/5 Pacotes"
sudo apt-get update -q >/dev/null || true
sudo apt-get install -y -q android-sdk-libsparse-utils erofs-utils python3 zstd binutils >/dev/null

msg "2/5 Tirando o super.img de dentro da ROM (demora)"
if [[ ! -e super.raw ]]; then
	mkdir -p rom
	tar -xzf "$ROM" -C rom --wildcards '*/images/super*' 2>/dev/null ||
		tar -xzf "$ROM" -C rom --wildcards '*super*'
	sup=$(find rom -name 'super.img*' | head -1)
	[[ -n $sup ]] || erro "não achei super.img na ROM"
	case $sup in
		*.zst) zstd -d -q "$sup" -o super.img; sup=super.img ;;
	esac
	if file "$sup" | grep -qi 'sparse'; then
		simg2img "$sup" super.raw
	else
		mv "$sup" super.raw
	fi
	rm -rf rom super.img
fi

msg "3/5 Separando as partições vendor e vendor_dlkm"
if [[ ! -f lpunpack.py ]]; then
	curl -fsSL -o lpunpack.py https://raw.githubusercontent.com/unix3dgforce/lpunpack/master/lpunpack.py
fi
mkdir -p parts
for p in vendor_a vendor_dlkm_a vendor vendor_dlkm; do
	[[ -s parts/$p.img ]] || python3 lpunpack.py -p "$p" super.raw parts/ >/dev/null 2>&1 || true
done
ls -lh parts/ || true
ls parts/*.img >/dev/null 2>&1 || erro "não consegui separar as partições do super.img"

msg "4/5 Abrindo as partições (EROFS)"
for img in parts/*.img; do
	n=$(basename "$img" .img)
	[[ -s $img ]] || continue
	rm -rf "fs/$n"; mkdir -p "fs/$n"
	fsck.erofs --extract="fs/$n" "$img" >/dev/null 2>&1 || echo "aviso: $n não é EROFS ou falhou"
done

msg "5/5 Juntando os arquivos do toque"
OUT=touch-android
rm -rf "$OUT"; mkdir -p "$OUT"
find fs -type f \( -iname '*focal*' -o -iname '*fts*' -o -iname '*ft5452*' -o -iname '*ft3*' \
	-o -iname '*touch*' -o -iname '*xiaomi_touch*' -o -iname '*tp_*' \) \
	! -path '*/app/*' ! -path '*/priv-app/*' ! -iname '*.apk' ! -iname '*.odex' ! -iname '*.vdex' -print0 |
	while IFS= read -r -d '' f; do
		sz=$(stat -c %s "$f")
		(( sz < 8000000 )) || continue
		mkdir -p "$OUT/$(dirname "${f#fs/}")"
		cp "$f" "$OUT/${f#fs/}"
	done
# Linhas de configuração que carregam módulos de toque
grep -rIl -i 'focal\|fts\|touch' fs/*/etc fs/*/lib/modules 2>/dev/null | while read -r f; do
	mkdir -p "$OUT/cfg/$(dirname "${f#fs/}")"
	cp "$f" "$OUT/cfg/${f#fs/}"
done || true
find fs -name 'modules.load*' -o -name 'modules.dep' | while read -r f; do
	mkdir -p "$OUT/cfg/$(dirname "${f#fs/}")"; cp "$f" "$OUT/cfg/${f#fs/}"
done || true
find "$OUT" -type f | sed "s|^$OUT/||" | sort > "$OUT/LISTA.txt"
tar -czf touch-android.tar.gz "$OUT"
ls -lh touch-android.tar.gz
echo
echo "Pronto. Mande o arquivo $WORK/touch-android.tar.gz no chat."
echo "Depois, se quiser liberar espaço: rm -rf '$WORK/super.raw' '$WORK/parts' '$WORK/fs'"
