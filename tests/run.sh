#!/bin/sh
# Testes offline do vkshim v4 (não precisa de GPU):
#   compila os shaders de teste, roda o passe de clip/cull, os passes Mali e as emulações da v4.7
#   (clip -> varying, depthClamp, gl_DrawID, discard no FS, GS do wireframe), valida com spirv-val.
set -e
cd "$(dirname "$0")"
SPVT=${SPVT:-../deps/SPIRV-Tools}
B=$(mktemp -d)
for s in clip.vert clip2.vert clip.geom bits.frag lock.comp plain.vert plain.frag; do
  glslangValidator -V --target-env vulkan1.1 -o "$B/$s.spv" "$s" >/dev/null
done
clang -O2 -o "$B/cctest" cctest.c ../shim/clipcull.c
clang++ -O2 -std=c++17 -I"$SPVT/include" -o "$B/spvtest" spvtest.cpp ../shim/spvfix.cpp \
  "$SPVT/build/source/opt/libSPIRV-Tools-opt.a" "$SPVT/build/source/libSPIRV-Tools.a"
"$B/cctest" "$B"/clip*.spv
"$B/spvtest" "$B"/bits.frag.spv "$B"/lock.comp.spv
fail=0
for f in "$B"/*.cc.spv "$B"/*.fix.spv; do
  [ -f "$f" ] || continue
  if spirv-val --target-env vulkan1.1 "$f"; then
    n=$(spirv-dis "$f" | grep -c "ClipDistance\|CullDistance" || true)
    case "$f" in *.cc.spv) [ "$n" = 0 ] || { echo "ainda tem Clip/CullDistance: $f"; fail=1; };; esac
  else echo "INVALIDO: $f"; fail=1; fi
done
# v4.7: emulações (spvemu) sobre VS/TES/GS/FS, inclusive SPIR-V 1.5
E=$B/emu; mkdir -p "$E"
for s in clipblk.vert clipv.vert clipgs.geom cliptes.tese drawid.vert drawidpc.vert tri.vert col.frag colv.frag fsloc.frag fragdepth.frag fragcoord.frag; do
  glslangValidator -V --target-env vulkan1.1 -o "$E/$s.spv" "$s" >/dev/null
done
glslangValidator -V --target-env vulkan1.2 -o "$E/clipblk15.vert.spv" clipblk.vert >/dev/null
glslangValidator -V --target-env vulkan1.2 -o "$E/drawidpc15.vert.spv" drawidpc.vert >/dev/null
clang -O2 -o "$B/emutest" emutest.c ../shim/spvemu.c ../shim/clipcull.c
(cd "$E" && "$B/emutest" *.spv) || { echo "emutest FALHOU"; fail=1; }
for f in "$E"/*.emu.spv "$E"/*.cnt.spv "$E"/*.emux.spv "$E"/*.fsclip.spv "$E"/*.fsclamp.spv "$E"/*.fsclampx.spv "$E"/*.new.spv "$E"/*.dnew.spv "$E"/*.gsl.spv "$E"/*.gsp.spv; do
  [ -f "$f" ] || continue
  spirv-val --target-env vulkan1.2 "$f" || { echo "INVALIDO: $f"; fail=1; }
  case "$f" in *.emu.spv) n=$(spirv-dis "$f" | grep -c "OpCapability ClipDistance" || true); [ "$n" = 0 ] || { echo "ainda tem ClipDistance: $f"; fail=1; };; esac
done
echo "spvemu: $(ls "$E"/*.emu.spv "$E"/*.cnt.spv "$E"/*.emux.spv "$E"/*.fsclip.spv "$E"/*.fsclamp.spv "$E"/*.fsclampx.spv "$E"/*.dnew.spv "$E"/*.new.spv "$E"/*.gsl.spv "$E"/*.gsp.spv 2>/dev/null | wc -l) módulos gerados"
rm -rf "$B"
[ $fail = 0 ] && echo "OK: todos os módulos válidos" || { echo "FALHOU"; exit 1; }
