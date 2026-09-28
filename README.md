# EDF6 Enemy HP

Vida de los enemigos que vas golpeando en Earth Defense Force 6. Cada enemigo tiene una tarjeta
compacta con el porcentaje, la barra, la vida actual / máxima y tu último golpe, en dos columnas y
hasta 6: primero el que estás golpeando, después los otros vivos y al final las kills. Una kill queda
4 segundos con cuánto tardaste en matarlo y tu DPS promedio. Los vivos se van a los 5 segundos sin
golpes. F3 apaga todo.

Un enemigo excepcional pasa a una tarjeta detallada arriba: barra de 10 segmentos, tu DPS de los
últimos 5 segundos y el tiempo estimado para matarlo a ese ritmo. Es excepcional si su vida máxima es
4 veces o más la mediana de los últimos 32 enemigos que golpeaste, o si lleva más de 4 segundos
aguantando tus golpes. Una vez excepcional, queda así.

Pingear un enemigo (la tecla de "spot", Q por defecto) lo pasa a la tarjeta detallada durante 30
segundos aunque no le pegues; cada ping renueva el plazo y la muerte lo corta. La vida sigue al día
con el daño de cualquiera, aliados incluidos.

El DPS cuenta solo tu daño, no el de aliados ni NPC. La franja clara de la barra es el daño reciente:
se acumula mientras disparás y se achica medio segundo después de que parás.

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

## Cómo se entera de un ping

El juego llama "spot" al ping: crea un objeto `SpotEffect` (los tres círculos son `SpotCircle.dds`)
cuyo `InitParam` trae el enemigo en `+0x40`. Esta DLL engancha el constructor de `SpotEffect` (hoy
`EDF.dll+0x3047c0`) y, si el objetivo es del equipo enemigo, lo marca por 30 segundos. La vida se lee
ahí mismo, mientras el juego construye el spot alrededor de ese enemigo.

Tampoco se busca por dirección: se parte del RTTI (`.?AVSpotEffect@@` → type descriptor → complete
object locator → vtable), se busca el `lea` que carga esa vtable y se toma la función que lo
contiene, que es el constructor. Si algo no coincide, lo dice en el log y el contador sigue sin
pings.

Se encontró con `hpscan.py ping_watch` (fotos del enemigo antes y después del ping, para separar lo
que se mueve solo), el RTTI de `EDF.dll` y `hpscan.py trace_exec` sobre el constructor.

## Diagnóstico

Una vez registrado, todo va a `Compendium.log` con el prefijo `hp enemigos:`. La línea
`modulos: registrado Enemy HP` confirma que el Compendium lo aceptó, `enganchado EDF.dll+0x...`
que encontró la función de daño, y `pings hooked at EDF.dll+0x...` que encontró la de los pings.

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
