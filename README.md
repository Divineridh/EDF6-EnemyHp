# EDF6 Enemy HP

HP of the enemies you hit in Earth Defense Force 6. Each enemy gets a compact card with the
percentage, the bar, current / max HP and your last hit, in two columns and up to 6: first the one
you're hitting, then the other live ones and last the kills. A kill stays for 4 seconds with how long
you took to kill it and your average DPS. Live ones go away after 5 seconds without hits. F3 turns
everything off.

An exceptional enemy moves to a detailed card on top: a 10-segment bar, your DPS over the last 5
seconds and the estimated time to kill it at that rate. It's exceptional if its max HP is 4 times or
more the median of the last 32 enemies you hit, or if it has lasted more than 4 seconds under your
fire. Once exceptional, it stays so.

Pinging an enemy (the game's "spot" key, Q by default) puts it on the detailed card for 30 seconds
even if you don't hit it; every ping renews the time and death ends it. Its HP stays up to date with
anyone's damage, allies included.

The DPS counts only your damage, not allies' or NPCs'. The light strip on the bar is recent damage:
it builds up while you shoot and shrinks half a second after you stop.

## Requirement: the Compendium

It's a module of [EDF6-Compendium](https://github.com/Divineridh/EDF6-Compendium): it brings no
drawing or keyboard hook of its own, but registers with the Compendium through
`Edf6Overlay_Register` (contract in `src/edf6_overlay_api.h`, version 2: it needs Compendium 0.3.0
or newer, which adds text with font and spacing). The Compendium tells it when you press the key,
through the same three paths it uses for F1, and lends it a surface to draw on every frame.

That way there's a single Present hook and a single set of input hooks, which is what took
stabilizing with the Steam overlay. This DLL only hooks the game's damage and ping functions, with
its own copy of MinHook: the Compendium doesn't touch those.

Without the Compendium installed it draws nothing and says so in `EnemyHp.log`, next to EDF6.exe.

## Install

```
EARTH DEFENSE FORCE 6\Mods\Plugins\EDF6EnemyHp.dll
```

The key is changed in `Mods\EnemyHp\config.ini` with a `key=0x72` line (virtual-key code; F3 by
default).

## How it finds HP

It hooks the function that applies all damage (today `EDF.dll+0x547c30`). It isn't looked up by
address: it's located by the `addss/minss/maxss/movss` block that writes HP, and the HP and max HP
offsets are read from those same instructions. A patch that moves code doesn't break it; if the shape
doesn't match, it doesn't hook and says so in the log.

The filter is by team, not by player pointer: team 0 player, 1 enemies, 2 allied NPCs (checked with
`trace_damage`). So it doesn't depend on the class or the vehicle. The enemy object is only read
inside the hook, while the game is using it, so a pointer to an enemy that was already freed is never
followed.

`tools/hpscan.py` is the tool it was found with: memory diff by known damage and hardware write and
execution breakpoints.

## How it learns about a ping

The game calls a ping a "spot": it creates a `SpotEffect` object (the three circles are
`SpotCircle.dds`) whose `InitParam` holds the enemy at `+0x40`. This DLL hooks `SpotEffect`'s
constructor (today `EDF.dll+0x3047c0`) and, if the target is on the enemy team, marks it for 30
seconds. HP is read right there, while the game builds the spot around that enemy.

It isn't looked up by address either: it starts from the RTTI (`.?AVSpotEffect@@` → type
descriptor → complete object locator → vtable), finds the `lea` that loads that vtable and takes the
function around it, which is the constructor. If something doesn't match, the log says so and the
counter keeps working without pings.

It was found with `hpscan.py ping_watch` (snapshots of the enemy before and after the ping, to
separate what moves on its own), `EDF.dll`'s RTTI and `hpscan.py trace_exec` on the constructor.

## Diagnostics

Once registered, everything goes to `Compendium.log` with the `enemy hp:` prefix. The line
`modules: registered Enemy HP` confirms the Compendium accepted it, `hooked EDF.dll+0x...` that it
found the damage function, and `pings hooked at EDF.dll+0x...` that it found the ping one.

## Build

```bash
build.bat
```

VS2019 Build Tools (MSVC 14.29). Dependencies aren't in the repo; clone them into `deps/`:

```bash
git clone https://github.com/TsudaKageyu/minhook    deps/minhook
git clone https://github.com/Quarri6343/EDF6Plugins deps/EDF6Plugins
```

## Package

```bash
python tools/package.py
```

Writes `EDF6EnemyHp.zip` to `../builds/` with the DLL, the sample `config.ini` and `README.txt`. It
refuses to package if any file in `src/` is newer than the DLL.

## Origin

It was born inside the Compendium's repo (`hp-enemigos` branch) and was split off when the Compendium
started accepting modules.
