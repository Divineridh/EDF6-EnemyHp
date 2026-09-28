import os
import sys
import zipfile

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FUENTE = os.path.join(RAIZ, "paquete")
DLL = os.path.join(RAIZ, "build", "EDF6EnemyHp.dll")
SALIDA = os.path.join(os.path.dirname(RAIZ), "builds")
ZIP = os.path.join(SALIDA, "EDF6EnemyHp.zip")

CONTENIDO = [
    (DLL, "Mods/Plugins/EDF6EnemyHp.dll"),
    (os.path.join(FUENTE, "config.ini"), "Mods/EnemyHp/config.ini"),
    (os.path.join(FUENTE, "LEEME.txt"), "LEEME.txt"),
]


def mas_nuevos_que_el_dll():
    corte = os.path.getmtime(DLL)
    pendientes = []
    for base, _, archivos in os.walk(os.path.join(RAIZ, "src")):
        for a in archivos:
            ruta = os.path.join(base, a)
            if os.path.getmtime(ruta) > corte:
                pendientes.append(os.path.relpath(ruta, RAIZ))
    return sorted(pendientes)


def main():
    faltan = [o for o, _ in CONTENIDO if not os.path.exists(o)]
    if faltan:
        raise SystemExit("faltan archivos para armar el paquete:\n  " + "\n  ".join(faltan))
    pendientes = mas_nuevos_que_el_dll()
    if pendientes:
        raise SystemExit("el DLL es mas viejo que el codigo; corre build.bat primero:\n  " + "\n  ".join(pendientes))

    os.makedirs(SALIDA, exist_ok=True)
    with zipfile.ZipFile(ZIP, "w", zipfile.ZIP_DEFLATED) as z:
        for origen, relativo in CONTENIDO:
            z.write(origen, relativo)

    print(ZIP)
    for origen, relativo in CONTENIDO:
        print("  %-36s %8d bytes" % (relativo, os.path.getsize(origen)))


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    main()
