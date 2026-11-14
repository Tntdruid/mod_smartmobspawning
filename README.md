# mod_smartmobspawning

AzerothCore-modul, der forkorter respawn-tiden på mobs i den åbne verden, mens en spiller i nærheden stadig mangler et quest-item fra deres loot. Jo færre af de relevante mobs der er i live omkring spilleren, jo hurtigere kommer de igen.

Modulet forkorter kun timere. Det lægger sig oven på core-indstillingerne `Respawn.Dynamic*`.

## Sådan virker det

Når en mob dør, tjekker modulet, om en spiller inden for `Radius` mangler noget fra dens loot. Hvis ja, beregnes ny respawn-tid ud fra, hvor mange relevante mobs der stadig lever:

```
upper = min(normal respawn-tid, MaxDelay)
delay = MinDelay + (upper - MinDelay) * min(1, levende / (AliveTargetPerPlayer * grupper))
```

En gruppe tæller som én, uanset hvor mange medlemmer og bots der mangler loot. En spiller uden gruppe tæller også som én.

Med standardværdierne og én spiller eller gruppe på en mob med 300 sekunders respawn:

| Levende mobs i nærheden | Ny respawn-tid |
|---|---|
| 0 | ca. 30 sek. |
| 2 | ca. 66 sek. |
| 5 eller flere | ca. 120 sek. |

Derudover scanner modulet hvert `ScanInterval` sekund køen af ventende respawns omkring spilleren. Det hjælper, når mobs døde, før du fik brug for dem, for eksempel før du tog questen. Der scannes én gang pr. gruppe, fra de rigtige spillere i den, og gruppemedlemmer inden for `Radius` tæller med i, hvad der mangles.

Der lægges en lille forskydning oven i tiden pr. spawn, så en hel flok ikke dukker op på samme tid.

## Gameobjects

Objekter med loot, for eksempel Giant Clam til questen Pearl Diving, behandles på samme måde og bruger de samme tider. Scanningen finder dem efter loot, så tiden regnes fra den scanning, op til `ScanInterval` sekunder senere. Med `ScanInterval = 0` ændres objekter ikke.

Giant Clam ligger i en pulje, hvor kun en del af spawn-punkterne er aktive ad gangen. Modulet forkorter ventetiden, men det er stadig puljen, der vælger, hvor den næste clam dukker op, så den kan komme et andet sted end den, du lige har lootet.

Urter og malmårer ændres ikke, medmindre `GameObjects.IncludeGatheringNodes = 1`.

## Hvad der ikke ændres

- Dungeons, raids og battlegrounds.
- Rare, rare elite og world bosses. Elites kun med `AllowElites = 1`.
- Quest givers.
- Spawns med normal respawn-tid over `MaxSpawnTime`.
- Spawns i pools, medmindre `IncludePools = 1`.
- Lig fjernes aldrig tidligere, så en mob kan ikke komme igen, før liget er forsvundet. Med standardindstillingerne er det 60 sekunder for en ulootet normal mob og højst 30 sekunder, når den er lootet.

## Installation

1. Kopiér mappen til `modules/mod_smartmobspawning` i din AzerothCore-kilde.
2. Kør CMake igen, og byg serveren.
3. Kopiér `mod_smartmobspawning.conf.dist` til `mod_smartmobspawning.conf` i din `etc/modules`-mappe.
4. Genstart worldserver. Indstillingerne kan senere genindlæses med `.reload config`.

Sæt `SmartMobSpawning.Debug = 1` for at se hver ændring i serverloggen.
