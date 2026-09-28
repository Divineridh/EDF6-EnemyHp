# EDF6 Enemy HP

Tarjeta con la vida del último enemigo que golpeaste en Earth Defense Force 6: porcentaje, barra de
10 segmentos que pasa de verde a ámbar y rojo, vida actual / máxima, tu DPS de los últimos 5
segundos, el tiempo estimado para matarlo a ese ritmo y el último golpe. El DPS cuenta solo tu daño,
no el de aliados ni NPC. Aparece al pegar, se va a los 5 segundos sin golpes (1,5 s si lo mataste)
y F3 la apaga.

## Requisito: el Compendium

Es un módulo de [EDF6-Compendium](https://github.com/Divineridh/EDF6-Compendium): no trae hook de
dibujado ni de teclado propio, sino que se registra en el Compendium por
`Edf6Overlay_Register` (contrato en `src/edf6_overlay_api.h`, versión 2: necesita el Compendium
0.3.0 o más nuevo, que suma texto con fuente y espaciado). El Compendium le avisa
cuando apretás la tecla —por los mismos tres caminos que usa para F1— y le presta una superficie
para dibujar en cada frame.

Así hay un solo hook de Present y un solo juego de hooks de input, que es lo que costó estabilizar
con el overlay de Steam. Esta DLL solo engancha la función de daño del juego, con su propia copia de
MinHook: el Compendium no toca esa función.

Sin el Compendium instalado no dibuja nada y lo deja escrito en `EnemyHp.log`, al lado del EDF6.exe.

## Instalar

```
EARTH DEFENSE FORCE 6\Mods\Plugins\EDF6EnemyHp.dll
```

La tecla se cambia en `Mods\EnemyHp\config.ini` con una línea `tecla=0x72` (código virtual; F3 por
defecto).

## Cómo encuentra la vida

Engancha la función que aplica todo el daño (hoy `EDF.dll+0x547c30`). No la busca por dirección:
la localiza por el bloque `addss/minss/maxss/movss` que escribe la vida, y lee los offsets de vida y
vida máxima de esas mismas instrucciones. Un parche que mueva código no la rompe; si la forma no
coincide, no engancha y lo dice en el log.

El filtro es por equipo y no por puntero al jugador: equipo 0 jugador, 1 enemigos, 2 NPC aliados
(verificado con `trace_damage`). Así no depende de la clase ni del vehículo. El objeto del enemigo
solo se lee adentro del hook, cuando el juego lo está usando, para no seguir un puntero de un
enemigo ya liberado.

`tools/hpscan.py` es la herramienta con la que se encontró: diff de memoria por daño conocido y
breakpoints de hardware de escritura y ejecución.

## Diagnóstico

Una vez registrado, todo va a `Compendium.log` con el prefijo `hp enemigos:`. La línea
`modulos: registrado Enemy HP` confirma que el Compendium lo aceptó, y `enganchado EDF.dll+0x...`
que encontró la función de daño.

## Compilar

```bash
build.bat
```

Build Tools de VS2019 (MSVC 14.29). Las dependencias no están en el repo; se clonan en `deps/`:

```bash
git clone https://github.com/TsudaKageyu/minhook    deps/minhook
git clone https://github.com/Quarri6343/EDF6Plugins deps/EDF6Plugins
```

## Empaquetar

```bash
python tools/paquete.py
```

Deja `EDF6EnemyHp.zip` en `../builds/` con la DLL, el `config.ini` de ejemplo y el `LEEME.txt`. Se
niega a empaquetar si algún archivo de `src/` es más nuevo que la DLL.

## Origen

Nació dentro del repo del Compendium (rama `hp-enemigos`) y se separó cuando el Compendium pasó a
aceptar módulos.
