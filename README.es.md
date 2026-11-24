<div align="center">

<h1>MMCTool para M5Stack Cardputer y Cardputer-Adv</h1>

<p><a href="README.md">🇬🇧 English</a> | <a href="README.ru.md">🇷🇺 Русский</a> | <b>🇪🇸 Español</b></p>

<p>
  <a href="https://platformio.org/"><img src="https://img.shields.io/badge/PlatformIO-ESP32--S3-orange?style=flat-square" alt="PlatformIO"></a>
  <a href="https://github.com/bmorcelli/Launcher"><img src="https://img.shields.io/badge/M5Launcher-Compatible-brightgreen?style=flat-square" alt="M5Launcher"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-GPLv3-blue?style=flat-square" alt="GPL-3.0"></a>
</p>

<p><i>Sintetizador polifónico de 6 voces, guitarra, arpa, piano y caja de ritmos breakcore/tekk para Cardputer y Cardputer-Adv. Sin PSRAM, con boost de 200% y grabación WAV en microSD.</i></p>

<p><a href="../../releases/latest"><b>⬇️ Descargar último .bin (M5Launcher y FullFlash 0x0)</b></a></p>

</div>

## Qué es MMCTool?

Quería un instrumento real de bolsillo en mi Cardputer-Adv donde pudiera mantener acordes, ver notas reales con sostenidos (`#`), aplicar reverb/eco y grabar ideas directo a la microSD sin PC. De ahí nació **MMCTool**.

Un solo `.bin` 2-en-1 funciona tanto en **M5Stack Cardputer-Adv** (DAC ES8311 + jack 3.5 mm) como en **M5Stack Cardputer** original.

## Funciones principales

- **10 instrumentos DSP en tiempo real (mantén 1.3s para cambiar sin toques accidentales):**
  - `1` **BASS** - bajo eléctrico potente con overdrive
  - `2` **E-GTR** - guitarra eléctrica con 3 niveles de distorsión (`CLN / CRN / HVY`)
  - `3` **LYRE** - arpa cristalina (síntesis física Karplus-Strong)
  - `4` **SYNTH** - sintetizador polifónico (`SAW / SQR / TRI / PAD / Phonk BELL` con `7`)
  - `5` **DRUMS** - 3 kits de batería con `7`: **AMEN** (breakcore, crash brillante, snare roll), **TEKK** (kicks hardtekk/gabber/piep + rave siren), **PHNK** (trap 808 + cowbell + láser)
  - `6` **DPAD** - modo MPC: fila superior con acordes instantáneos (`Am, F, C, G, Dm, Em, E7, A7`), fila media con bajo 808 y fila inferior con ritmos
  - `Esc` **PIANO** - piano acústico (activa automáticamente el layout `PNO`)
  - `Tab` **REESE** - bajo Reese estilo DnB / Sewerslvt
  - `Fn` **CHAIN** - Chainsaw Supersaw de 4 osciladores para EDM / Hyperpop
  - `Ctrl` **ACID** - sintetizador ácido resonante TB-303
- **Notación musical real:**
  - Nombres reales de notas (`C, C#, D, F#...`) en cada pad
  - Tónica resaltada con doble anillo, sostenidos (`#`) con estilo de tecla negra y número de octava a la izquierda
  - Tonalidad actual arriba (`K:Am`, `K:F#m`) + detector de acordes en vivo (`[Am] A3 C4 E4`)
  - Cambio rápido de octava `-3..+3` con `-` / `=` o el botón lateral `G0` (`1x` subir, `2x` bajar)
- **5 layouts (mantén `9` / `0`):** `MAJ` (estilo lira de Genshin), `MIN` (menor emocional), `PNO` (piano con sostenidos `#` arriba), `BIG` (teclas dobles anchas), `CHD` (rasgueo de acordes con una tecla).
- **Efectos en vivo y volumen al 200%:** Reverb (`O`), Eco estéreo (`P`), Vibrato/Trémolo (`K`), Filtro (`L`), bloqueo Sustain con 1 toque (`M`) y volumen en 30 pasos hasta `200%` (`[` / `]`) con boost automático del ES8311.
- **Grabadora WAV en microSD y reproducción:**
  - Guarda WAV estéreo en `/MMCTool_Rec/` sin mezclarse con tu música
  - Mantén `\` para grabar/detener, mantén `Enter` para reproducir (puedes tocar encima en vivo), **Toque + Mantener `Enter`** abre el menú de grabaciones, `Del` detiene la reproducción al instante.
- **Modo GUIDE (entrenador tipo Synthesia):** mantén `/` para seguir notas paso a paso desde `/MMCTool_Songs/*.txt`. Incluye `index.html` para convertir `.mid` / `.mp3` / `.wav` desde el navegador.
- **4 temas completos y 4 pantallas de inicio únicas (guardadas en NVS):** `RETRO 84`, `AMBER CRT`, `CYBER 2077` y `GHOST WHITE`. Cambia con `G0` x3 o mantén `'`.

## Instalación

- **`MMCTool_M5Launcher.bin`** - cópialo a la microSD e instálalo desde [M5Launcher](https://github.com/bmorcelli/Launcher) de @bmorcelli.
- **`MMCTool_FullFlash_0x0.bin`** - flasheo directo por USB en offset `0x0` con M5Burner o Web ESPTool.

## Controles rápidos

| Tecla | Acción |
| --- | --- |
| `Q..I` / `Aa..J` / `Opt..N` | Cuadrícula recta 3x8 (24 pads) con ondas que se disuelven |
| Mantener `1`..`6` / `Esc` / `Tab` / `Fn` / `Ctrl` | Cambiar instrumento (bloqueo de 1.3s) |
| Mantener `7` | Kit de batería (`AMEN/TEKK/PHNK`), onda o distorsión |
| Mantener `8` / Mantener `9`,`0` | Visualizador (Ondas / Osciloscopio) / Layout |
| `-` / `=` o `G0` (1x / 2x) | Cambiar octava `-3..+3` |
| `[` / `]` | Volumen 0..200% (30 pasos + boost ES8311) |
| Mantener `O` / `P` / `K` / `L` | Reverb / Echo / Mod / Filtro |
| `M` / `Space` | Bloqueo Sustain (1 toque) / Pedal Sustain |
| Mantener `\` / Mantener `Enter` / `Del` | Grabar WAV / Reproducir-Detener / Parada forzada |
| Toque + Mantener `Enter` | Abrir menú de grabaciones `/MMCTool_Rec` |
| Mantener `/` | Activar entrenador `GUIDE` (`/MMCTool_Songs/*.txt`) |
| `G0` (3x) o Mantener `'` | Cambiar tema (se guarda al reiniciar) |

## Apoyo y desarrollo a medida

Este proyecto es gratis y open-source.  
Por desgracia, las facturas no se pagan solas 💀

Si te sirvió MMCTool o quieres funciones personalizadas, temas propios o un firmware a medida para tu Cardputer, escríbeme y lo armamos por un precio accesible:

- USDT (TRC20): TF7kf8QxKoXrHq8ixnaYG5zDXBBuRP5FWb 
- BTC: bc1qef8762lcc72zxz5w6kac4vztpjuw5vxlxaal5c
- **Contacto:** Farbender@protonmail.com

## Licencia

GNU General Public License v3.0 (GPL-3.0).
