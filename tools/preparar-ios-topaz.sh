#!/bin/bash
# preparar-ios-topaz.sh — prepara tudo para rodar o iOS 16 (ios-topaz) no Redmi Note 12 4G.
#
# Uso (no Ubuntu do pendrive, modo "Experimentar"):
#   bash preparar-ios-topaz.sh PASTA_DE_TRABALHO CAMINHO_DO_IPSW CAMINHO_DO_xnu-kernel.zip
#
# Exemplo:
#   bash preparar-ios-topaz.sh /media/ubuntu/Windows/ios \
#        /media/ubuntu/Windows/Users/Denis/Downloads/iPhone10,3,iPhone10,6_16.7.16_20H392_Restore.ipsw \
#        /media/ubuntu/Windows/Users/Denis/Downloads/xnu-kernel.zip
#
# A PASTA_DE_TRABALHO precisa estar num disco de verdade (NÃO na RAM do pendrive)
# e ter pelo menos 45 GB livres.
#
# No fim, a pasta PASTA_DE_TRABALHO/saida terá:
#   shim-teste.img    -> boot de teste, disco só leitura (não grava nada no celular)
#   shim-ios.img      -> boot de verdade do iOS
#   ios16-root.simg   -> sistema iOS para gravar na partição userdata
#
# O script NÃO mexe no celular. Gravar e dar boot são comandos manuais.

set -euo pipefail

# ---------------------------------------------------------------- argumentos
if [[ $# -ne 3 ]]; then
	sed -n '2,20p' "$0"
	exit 2
fi
WORK=$(realpath -m "$1")
IPSW=$(realpath "$2")
KZIP=$(realpath "$3")

msg()  { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
erro() { printf '\n\033[1;31mERRO: %s\033[0m\n' "$*" >&2; exit 1; }

[[ -f $IPSW ]] || erro "IPSW não encontrado: $IPSW"
[[ -f $KZIP ]] || erro "xnu-kernel.zip não encontrado: $KZIP"
[[ $(uname -m) == x86_64 ]] || erro "este script é para PC Intel/AMD de 64 bits"

mkdir -p "$WORK" || erro "não consegui criar $WORK (o disco do Windows está só leitura? desative a Inicialização Rápida)"
touch "$WORK/.teste-escrita" 2>/dev/null || erro "não consigo gravar em $WORK (Inicialização Rápida do Windows ou BitLocker)"
rm -f "$WORK/.teste-escrita"

livre_gb=$(( $(df --output=avail -k "$WORK" | tail -1) / 1024 / 1024 ))
if (( livre_gb < 45 )); then
	erro "só ${livre_gb} GB livres em $WORK; precisa de pelo menos 45 GB"
fi
msg "Pasta de trabalho: $WORK (${livre_gb} GB livres)"

TOOLS="$WORK/ferramentas"
REPO="$WORK/ios-topaz"
IPSWX="$WORK/ipsw-extraido"
SAIDA="$WORK/saida"
mkdir -p "$TOOLS" "$IPSWX" "$SAIDA"

# ---------------------------------------------------------------- 1. pacotes
msg "1/8 Instalando pacotes do Ubuntu"
# Remove o pacote "ipswd" (o daemon), instalado por engano numa versão anterior deste script.
# Os scripts dele falham no modo de teste do Ubuntu e travam o apt, então são apagados antes.
if dpkg -s ipswd >/dev/null 2>&1 || [[ -e /var/lib/dpkg/info/ipswd.list ]]; then
	sudo rm -f /var/lib/dpkg/info/ipswd.postinst /var/lib/dpkg/info/ipswd.prerm /var/lib/dpkg/info/ipswd.postrm
	sudo dpkg --purge --force-remove-reinstreq ipswd || sudo dpkg --remove --force-all ipswd || true
fi
curl -fsS --max-time 15 -o /dev/null https://github.com ||
	erro "sem internet (não consegui acessar github.com). Conecte o PC na internet e rode de novo."
# Ativa o repositório "universe" sem depender do add-apt-repository (que pode dar timeout)
if ! grep -rqs 'universe' /etc/apt/sources.list /etc/apt/sources.list.d/; then
	if [[ -f /etc/apt/sources.list.d/ubuntu.sources ]]; then
		sudo sed -i -E '/^Components:/{/universe/!s/$/ universe/}' /etc/apt/sources.list.d/ubuntu.sources
	else
		sudo add-apt-repository -y universe >/dev/null || echo "aviso: não consegui ativar o universe; tentando assim mesmo"
	fi
fi
sudo apt-get update -q
sudo apt-get install -y -q git python3 rsync unzip cpio curl hfsprogs \
	android-sdk-libsparse-utils fastboot \
	gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu build-essential \
	cmake fuse3 libfuse3-dev zlib1g-dev libbz2-dev libattr1-dev

# ---------------------------------------------------------------- 2. apfs-fuse
msg "2/8 apfs-fuse (lê os discos da Apple)"
if ! command -v apfs-fuse >/dev/null; then
	rm -rf "$TOOLS/apfs-fuse"
	git clone --recursive -q https://github.com/sgan81/apfs-fuse.git "$TOOLS/apfs-fuse"
	cmake -S "$TOOLS/apfs-fuse" -B "$TOOLS/apfs-fuse/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "-DCMAKE_CXX_FLAGS=-include cstdint" >/dev/null
	make -C "$TOOLS/apfs-fuse/build" -j"$(nproc)" >/dev/null
	sudo install -m 0755 "$TOOLS/apfs-fuse/build/apfs-fuse" /usr/local/bin/apfs-fuse
fi
command -v apfs-fuse >/dev/null || erro "apfs-fuse não instalou"

# ---------------------------------------------------------------- 3. ipsw
msg "3/8 Ferramenta ipsw (blacktop/ipsw)"
if ! command -v ipsw >/dev/null; then
	# Usa o .tar.gz da ferramenta de linha de comando "ipsw" (NÃO o "ipswd"), sem passar pelo dpkg
	url=$(curl -fsSL https://api.github.com/repos/blacktop/ipsw/releases/latest |
		grep -o '"browser_download_url": *"[^"]*/ipsw_[^"/]*_linux_x86_64\.tar\.gz"' | head -1 | cut -d'"' -f4)
	[[ -n $url ]] || erro "não achei o ipsw_..._linux_x86_64.tar.gz nas releases do GitHub"
	echo "Baixando: $url"
	rm -rf "$TOOLS/ipsw-bin"
	mkdir -p "$TOOLS/ipsw-bin"
	curl -fsSL -o "$TOOLS/ipsw.tar.gz" "$url"
	tar -xzf "$TOOLS/ipsw.tar.gz" -C "$TOOLS/ipsw-bin"
	bin=$(find "$TOOLS/ipsw-bin" -type f -name ipsw | head -1)
	[[ -n $bin ]] || erro "o pacote do ipsw não tem o executável 'ipsw'"
	sudo install -m 0755 "$bin" /usr/local/bin/ipsw
fi
command -v ipsw >/dev/null || erro "ipsw não instalou"

# ---------------------------------------------------------------- 4. mkbootimg
msg "4/8 mkbootimg (AOSP)"
if [[ ! -f $TOOLS/mkbootimg/mkbootimg.py ]]; then
	rm -rf "$TOOLS/mkbootimg"
	git clone -q --depth 1 https://android.googlesource.com/platform/system/tools/mkbootimg "$TOOLS/mkbootimg"
fi
# O shim/build.sh usa $MKBOOTIMG sem aspas: o caminho não pode ter espaços (ex.: "Denis Menezes").
# Copia o mkbootimg para /opt, que não tem espaço no caminho.
sudo rm -rf /opt/mkbootimg
sudo cp -r "$TOOLS/mkbootimg" /opt/mkbootimg
export MKBOOTIMG="python3 /opt/mkbootimg/mkbootimg.py"

# ---------------------------------------------------------------- 5. projeto + kernel
msg "5/8 Baixando o ios-topaz e colocando o kernel"
if [[ ! -d $REPO/.git ]]; then
	git clone -q --depth 1 -b topaz https://github.com/MakrSas/ios-topaz.git "$REPO"
fi
git -C "$REPO" log --oneline -1
# Corrige o mkiosroot.sh para o Ubuntu 26.04: o "install" novo (uutils) recusa donos numéricos (ex.: 272)
python3 - "$REPO/tools/mkiosroot.sh" <<'PATCHER'
import sys
p = sys.argv[1]
s = open(p).read()
if 'PATCH-UUTILS' in s:
    print('mkiosroot.sh já corrigido'); sys.exit(0)
start = s.index('sudo install -d -o 272 -g 272 -m 0751')
end_marker = 'sudo install -m 0600 -o 0 -g 0 /dev/stdin "$work/root/private/var/keybags/systembag.kb"\n'
end = s.index(end_marker) + len(end_marker)
new = r'''# PATCH-UUTILS: o "install" do Ubuntu 26.04 (uutils) recusa donos numéricos como 272.
sudo python3 - "$db" "$work/root/private/var/keybags" <<'PY'
import os, sys
db, kb = sys.argv[1], sys.argv[2]
def mkd(p, mode, uid, gid):
    os.makedirs(p, exist_ok=True)
    os.chown(p, uid, gid)
    os.chmod(p, mode)
mkd(os.path.join(db, 'diagnostics'), 0o751, 272, 272)
for d in ('uuidtext', 'diagnostics/timesync'):
    mkd(os.path.join(db, d), 0o755, 272, 272)
for d in ('diagnostics', 'uuidtext', 'diagnostics/timesync'):
    f = os.path.join(db, d, '.migrated')
    open(f, 'w').close()
    os.chown(f, 272, 272)
    os.chmod(f, 0o644)
mkd(kb, 0o700, 0, 0)
f = os.path.join(kb, 'systembag.kb')
with open(f, 'w') as fh:
    fh.write('<?xml version="1.0" encoding="UTF-8"?>\n'
             '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n'
             '<plist version="1.0"><dict><key>KeyBagKeys</key><data>U1dFRVQ=</data></dict></plist>\n')
os.chown(f, 0, 0)
os.chmod(f, 0o600)
PY
'''
s = s[:start] + new + s[end:]
# fsck de verificação: um "Volume header needs minor repair" depois do reparo é inofensivo
s = s.replace('fsck.hfsplus -f -n "$out"\n', 'fsck.hfsplus -f -y "$out" >/dev/null || true\nfsck.hfsplus -f -n "$out" || echo "aviso: fsck pediu reparo menor no cabeçalho (inofensivo)"\n', 1)
open(p, 'w').write(s)
print('mkiosroot.sh corrigido')
PATCHER
mkdir -p "$REPO/xnu/out"
unzip -o -q "$KZIP" -d "$REPO/xnu/out"
KERNEL=$(find "$REPO/xnu/out" -type f -name 'kernel.development.*' ! -name '*.dSYM*' | head -1)
[[ -n $KERNEL ]] || erro "não achei kernel.development.* dentro do xnu-kernel.zip"
echo "Kernel: $KERNEL"

# ---------------------------------------------------------------- 6. IPSW -> imagem do iOS
msg "6/8 Extraindo o IPSW (iPhone X, D22AP)"
unzip -o -q "$IPSW" BuildManifest.plist -d "$IPSWX"
mapfile -t PARTES < <(python3 - "$IPSWX/BuildManifest.plist" <<'PY'
import plistlib, sys
m = plistlib.load(open(sys.argv[1], 'rb'))
for bi in m['BuildIdentities']:
    info = bi.get('Info', {})
    if info.get('DeviceClass', '').lower() == 'd22ap' and info.get('RestoreBehavior') == 'Erase':
        man = bi['Manifest']
        for k in ('OS', 'Cryptex1,SystemOS', 'Cryptex1,AppOS', 'DeviceTree'):
            print(man[k]['Info']['Path'])
        break
else:
    sys.exit('identidade D22AP/Erase não encontrada no BuildManifest')
PY
)
[[ ${#PARTES[@]} -eq 4 ]] || erro "BuildManifest inesperado (este é o IPSW do iPhone X 16.7.16?)"
OS_DMG=${PARTES[0]} SYS_DMG=${PARTES[1]} APP_DMG=${PARTES[2]} DT_IM4P=${PARTES[3]}
echo "OS:        $OS_DMG"
echo "SystemOS:  $SYS_DMG"
echo "AppOS:     $APP_DMG"
echo "DeviceTree:$DT_IM4P"

if [[ -f $SAIDA/ios16-root.simg ]]; then
	echo "ios16-root.simg já existe; pulando a montagem da imagem."
else
	unzip -o -q "$IPSW" "$OS_DMG" "$SYS_DMG" "$APP_DMG" -d "$IPSWX"
	msg "Montando a imagem HFS+ de 16 GB (demora bastante)"
	rm -f "$WORK/ios16-root.img"
	( cd "$REPO" && sudo bash tools/mkiosroot.sh "$IPSWX/$OS_DMG" "$IPSWX/$SYS_DMG" "$IPSWX/$APP_DMG" "$WORK/ios16-root.img" 16G )
	sudo mkdir -p /mnt/iosroot
	sudo mount -t hfsplus -o loop,force "$WORK/ios16-root.img" /mnt/iosroot
	( cd "$REPO" && sudo python3 tools/sweet-root-tweaks.py /mnt/iosroot ) || { sudo umount /mnt/iosroot; erro "sweet-root-tweaks falhou"; }
	sudo umount /mnt/iosroot
	msg "Convertendo para imagem sparse (para o fastboot)"
	img2simg "$WORK/ios16-root.img" "$SAIDA/ios16-root.simg"
	# libera espaço: a imagem crua e os .dmg não são mais necessários
	rm -f "$WORK/ios16-root.img" "$IPSWX/$OS_DMG" "$IPSWX/$SYS_DMG" "$IPSWX/$APP_DMG"
fi

# ---------------------------------------------------------------- 7. device tree do iPhone X
msg "7/8 Device tree do iPhone X"
mkdir -p "$REPO/research/ipsw-x"
ADT="$REPO/research/ipsw-x/d22ap.adt"
if [[ ! -f $ADT ]]; then
	unzip -o -q "$IPSW" "$DT_IM4P" -d "$IPSWX"
	tmp="$IPSWX/dt-extraido"
	rm -rf "$tmp"
	ipsw img4 im4p extract "$IPSWX/$DT_IM4P" -o "$tmp"
	if [[ -d $tmp ]]; then
		f=$(find "$tmp" -type f | head -1)
		[[ -n $f ]] || erro "ipsw não extraiu a device tree"
		cp "$f" "$ADT"
	else
		cp "$tmp" "$ADT"
	fi
fi
ls -l "$ADT"

# ---------------------------------------------------------------- 8. imagens de boot
msg "8/8 Montando as imagens de boot"
BASE='debug=0xc serial=3 cpus=4 keepsyms=1 -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin launchd_unsecure_cache=1 wdt=-1 sweetfb_noconsole=1 sweetfb_hz=30'

cd "$REPO"
mkdir -p shim/out
python3 tools/mkadt.py shim/out/topaz.adt --highmem

# teste seco: SEM ufsrw=1 rootrw=1 (disco só leitura) e com o texto do kernel visível
python3 tools/mkpayload.py "$KERNEL" shim/out/topaz.adt shim/out/payload-teste.bin \
	--cmdline "${BASE/ sweetfb_noconsole=1/}"
bash shim/build.sh "$REPO/shim/out/payload-teste.bin"
cp shim/out/shim-boot.img "$SAIDA/shim-teste.img"

# boot de verdade
python3 tools/mkpayload.py "$KERNEL" shim/out/topaz.adt shim/out/payload.bin \
	--cmdline "$BASE ufsrw=1 rootrw=1"
bash shim/build.sh "$REPO/shim/out/payload.bin"
cp shim/out/shim-boot.img "$SAIDA/shim-ios.img"

msg "PRONTO! Arquivos em $SAIDA:"
ls -lh "$SAIDA"
cat <<EOF

Próximos passos (celular em fastboot: Power + Volume -):
  1) Teste seco, não grava nada:
       sudo fastboot boot "$SAIDA/shim-teste.img"
  2) Se o teste passar, gravar o iOS (APAGA o Android) e dar boot:
       sudo fastboot -S 512M flash userdata "$SAIDA/ios16-root.simg"
       sudo fastboot boot "$SAIDA/shim-ios.img"
EOF
