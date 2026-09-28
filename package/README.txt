EDF6 Enemy HP
=============

HP of the enemies you hit. Each one gets a small card with its percentage,
bar, current / max HP and your last hit, up to 6 on screen. Kills stay for 4
seconds with how long you took and your average DPS.

Exceptional enemies (much more HP than what you've been fighting, or lasting
more than 4 seconds) get a big card on top, with your DPS (your damage only)
and the estimated time to kill them.

Pinging an enemy (Q by default) puts it on the big card for 30 seconds, even
if you don't hit it. Pinging it again renews the time, and if it dies the card
goes away.

F3 hides or shows everything.

REQUIREMENTS
------------
  - EDFModLoader: https://github.com/BlueAmulet/EDFModLoader
  - EDF6 Compendium v0.3.0 or newer:
    https://github.com/Divineridh/EDF6-Compendium/releases

This mod is a Compendium module: the Compendium draws on screen and reads the
key. Without the Compendium installed nothing shows up.

INSTALL
-------
Copy the Mods folder from this package over the game's, the one next to
EDF6.exe. It ends up like this:

    EARTH DEFENSE FORCE 6\Mods\Plugins\EDF6EnemyHp.dll
    EARTH DEFENSE FORCE 6\Mods\EnemyHp\config.ini

KEY
---
F3 by default. Change it in Mods\EnemyHp\config.ini: remove the # from the
"key" line and put the code of the key you want.

IF IT DOESN'T SHOW UP
---------------------
With the Compendium installed, everything is written to Compendium.log, next
to EDF6.exe:

  - "modules: registered Enemy HP"      the Compendium accepted it
  - "enemy hp: hooked EDF.dll..."       it found the damage function
  - "enemy hp: pings hooked at ..."     it found the ping one

If the first one is missing, check that the Compendium is v0.3.0 or newer. If
the second one is missing, the line before it says what didn't match (it can
happen if a game update changed that function).

If the Compendium isn't installed, the message goes to EnemyHp.log.
