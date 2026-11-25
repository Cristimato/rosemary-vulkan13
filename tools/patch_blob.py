#!/usr/bin/env python3
"""Aplica os patches binarios no blob Mali r49p1 (donor MT6768 / Mali-G52)
para que ele rode como driver Vulkan na Mali-G76 (MT6785) ao lado do r32p1.

Uso: patch_blob.py <dir_do_zip_donor_extraido> <dir_saida_lib64>

Cada patch confere os bytes originais antes de escrever; se o blob for de
outra versao, o script aborta em vez de gerar um binario corrompido.
"""
import hashlib
import sys
from pathlib import Path

PATCHES = {
    # libGLES_mali.so -> libGLES_ma49.so
    "egl/libGLES_mali.so": [
        # SONAME/dynstr "libGLES_mali.so" -> "libGLES_ma49.so" (mesmo tamanho);
        # permite carregar junto com o libGLES_mali.so r32p1 do sistema.
        (0x2490E, b"li", b"49"),
        # Checagem de GPU ID: o DDK foi compilado p/ 0x7000002 (G52) e recusa
        # 0x7211 (G76). Mesmo ISA Bifrost v7 -> forca "compativel":
        #   cmp w26,w27 ; b.ne  ->  mov w0,#1 ; b +0xb8
        (0x1F29624, bytes.fromhex("5f031b6b81010054"), bytes.fromhex("200080522d000014")),
    ],
    # HAL Vulkan: NEEDED libGLES_mali.so -> libGLES_ma49.so
    "hw/vulkan.mali.so": [
        (0x388, b"li", b"49"),
    ],
}

# arquivo de origem -> nome no modulo (relativo a lib64/)
LAYOUT = {
    "egl/libGLES_mali.so": "libGLES_ma49.so",
    "hw/vulkan.mali.so": "hw/vulkan.r49.so",
    "libgp49.so": "libgp49.so",
    "libgpd9.so": "libgpd9.so",
    "android.hardware.common-V2-ndk.so": "android.hardware.common-V2-ndk.so",
    "android.hardware.graphics.common-V5-ndk.so": "android.hardware.graphics.common-V5-ndk.so",
}


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 1
    src = Path(sys.argv[1]) / "system/vendor/lib64"
    dst = Path(sys.argv[2])
    for rel, out in LAYOUT.items():
        data = bytearray((src / rel).read_bytes())
        for off, old, new in PATCHES.get(rel, []):
            cur = bytes(data[off:off + len(old)])
            if cur == new:
                print(f"  {rel}+{off:#x}: ja aplicado")
                continue
            if cur != old:
                print(f"ERRO {rel}+{off:#x}: esperado {old.hex()}, achei {cur.hex()} (blob diferente?)")
                return 2
            data[off:off + len(new)] = new
            print(f"  {rel}+{off:#x}: {old.hex()} -> {new.hex()}")
        target = dst / out
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        print(f"{out:45s} sha256 {hashlib.sha256(data).hexdigest()[:16]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
