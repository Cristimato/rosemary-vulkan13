#!/bin/sh
# Testes offline do vkshim v4 (não precisa de GPU):
#   compila os shaders de teste, roda o passe de clip/cull e os passes Mali, valida com spirv-val.
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
rm -rf "$B"
[ $fail = 0 ] && echo "OK: todos os módulos válidos" || { echo "FALHOU"; exit 1; }
