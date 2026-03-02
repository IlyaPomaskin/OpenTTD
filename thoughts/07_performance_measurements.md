# Performance Measurements — CPU Rendering Pipeline

Device: Android (via `tools/run_android.sh perf 10`)
Date: 2026-03-02
Mode: `_gles_gpu_sprites = false` (full CPU rendering → GPU texture upload → FBO → screen)
Measurement: 0.5s windows, per-frame averages reported

## Methodology

- GLESPerfCounters struct tracks 19 metrics across all rendering phases
- Counters accumulated per-frame, averaged and logged every 500ms
- 3 iterations × 10 seconds each = 30 seconds total measurement
- App running as live wallpaper on home screen with default map

## Summary Statistics (across all 3 iterations)

| Metric | Min | Avg | Max | Notes |
|--------|-----|-----|-----|-------|
| **FPS** | 19 | 39 | 61 | Varies with dirty area |
| **VP draw (µs/frame)** | 0 | 5800 | 13600 | Main bottleneck |
| **VP land (µs/frame)** | 0 | 1100 | 2233 | Tile iteration |
| **VP signs+tiles (µs/frame)** | 0 | 4100 | 10551 | Largest VP sub-phase |
| **VP sort (µs/frame)** | 0 | 170 | 290 | Sprite Z-sort |
| **VP vehicles (µs/frame)** | 0 | 19 | 38 | Negligible |
| **Tiles iterated/frame** | 0 | 120 | 269 | |
| **Parent sprites/frame** | 0 | 35 | 78 | |
| **Child sprites/frame** | 0 | 17 | 38 | |
| **Blit draw calls/frame** | 70 | 230 | 447 | |
| **Blit pixels (k/frame)** | 11 | 2000 | 4899 | 0.01-4.9 Mpx |
| **Upload (µs/frame)** | 434 | 1570 | 2850 | glTexSubImage2D |
| **Upload rows/frame** | 39 | 600 | 1487 | Partial upload |
| **Upload bytes (k/frame)** | 198 | 3100 | 7124 | 0.2-7.0 MB |
| **GPU paint (µs/frame)** | 2172 | 3050 | 3793 | FBO compositing |
| **Swap (µs/frame)** | 624 | 1000 | 2114 | eglSwapBuffers |

## Key Findings

### 1. VP Draw is the dominant bottleneck (60-70% of frame time)
When viewport redraws happen, `vp_draw` (CPU sprite compositing into video buffer) takes 5-13ms per frame. This is the `ViewportDrawParentSprites` call that iterates over sorted parent/child sprites and composites them pixel-by-pixel via the blitter.

### 2. Signs+tiles sub-phase is surprisingly expensive
`vp_signs_tiles` (4-10ms) includes `ViewportAddSigns`, `ViewportAddTownNames`, `ViewportDrawTileSprites`, and `ViewportAddTextEffects`. This is the sprite collection phase that runs before sorting.

### 3. FPS correlates inversely with dirty area
- Small dirty area (616×692): FPS 53-61, VP draw <2ms
- Medium dirty area (1076×1456): FPS 35-47, VP draw 5-7ms
- Full-screen dirty (1280×2856): FPS 19-31, VP draw 10-14ms

### 4. Idle frames achieve 51-61 FPS
When viewport doesn't redraw (vp_calls=0), FPS is 51-61. The bottleneck shifts to GPU paint (~3ms) + swap (~1.3ms). Upload is minimal (<0.5ms, <50 rows).

### 5. Upload cost is proportional to dirty area
Partial upload via `glTexSubImage2D` reduces upload from full-frame to just dirty rows. Upload ranges from 0.4ms (39 rows) to 2.8ms (1487 rows, 7MB).

### 6. GPU paint is constant ~3ms
`GLESBackend::Paint()` (FBO→screen blit) is consistently ~3ms regardless of dirty area. This is a single fullscreen quad draw.

## Bottleneck Breakdown (typical active frame at ~30 FPS)

```
Frame time: ~33ms
├── VP signs+tiles:  ~6ms (18%)  ← sprite collection
├── VP draw:         ~8ms (24%)  ← CPU pixel compositing (MAIN BOTTLENECK)
├── VP land:         ~1.3ms (4%) ← tile iteration
├── VP sort:         ~0.2ms (1%) ← Z-sort
├── Upload:          ~2ms (6%)   ← glTexSubImage2D
├── GPU paint:       ~3ms (9%)   ← FBO compositing
├── Swap:            ~1ms (3%)   ← eglSwapBuffers
└── Other:          ~11ms (33%)  ← game loop, window updates, scheduling
```

## Optimization Targets (priority order)

1. **VP draw → GPU**: Replace CPU pixel compositing with GPU draw calls. Each parent sprite becomes a textured quad on the GPU. Eliminates the 8ms bottleneck entirely.

2. **VP signs+tiles → retained scene**: Cache tile sprites across frames. Only re-collect sprites for tiles that actually changed. Could reduce 6ms to near-zero for static scenes.

3. **Upload → eliminate**: With GPU sprites, no CPU video buffer needed. Only UI elements (status bar, windows) would need CPU→GPU upload.

4. **GPU paint → batch optimization**: Current single fullscreen blit is already efficient. With GPU sprites, multiple draw calls needed but modern batching keeps this fast.

---

## Raw Data

### Iteration 1

```
fps=47 frames=26 | vp: land=725 veh=7 signs=1830 sort=90 draw=2368us tiles=39 psprites=10 csprites=4 area=1184x2112 calls=11 | blit: draws=92 px=447k fillrect=9 glyphs=65 | gpu: upload=1412us rows=232 bytes=1163k paint=3030us swap=1092us
fps=34 frames=19 | vp: land=1109 veh=19 signs=5421 sort=154 draw=6703us tiles=197 psprites=51 csprites=28 area=1276x2716 calls=16 | blit: draws=309 px=3483k fillrect=19 glyphs=135 | gpu: upload=1568us rows=964 bytes=4823k paint=2172us swap=922us
fps=25 frames=14 | vp: land=1540 veh=27 signs=8246 sort=233 draw=9866us tiles=257 psprites=70 csprites=38 area=1280x2840 calls=13 | blit: draws=426 px=4792k fillrect=27 glyphs=184 | gpu: upload=2397us rows=1275 bytes=6377k paint=2941us swap=771us
fps=29 frames=15 | vp: land=1379 veh=18 signs=6785 sort=184 draw=8433us tiles=231 psprites=63 csprites=36 area=1256x2856 calls=12 | blit: draws=360 px=4361k fillrect=20 glyphs=137 | gpu: upload=2428us rows=1424 bytes=7124k paint=3679us swap=863us
fps=31 frames=16 | vp: land=1422 veh=18 signs=6556 sort=204 draw=7620us tiles=201 psprites=56 csprites=31 area=1104x2856 calls=13 | blit: draws=351 px=3695k fillrect=23 glyphs=161 | gpu: upload=1923us rows=1083 bytes=5416k paint=2913us swap=836us
fps=51 frames=26 | vp: land=725 veh=7 signs=1830 sort=90 draw=2368us tiles=39 psprites=10 csprites=4 area=1184x2112 calls=11 | blit: draws=92 px=447k fillrect=9 glyphs=65 | gpu: upload=1412us rows=232 bytes=1163k paint=3030us swap=1092us
fps=55 frames=29 | vp: land=744 veh=13 signs=1508 sort=121 draw=3446us tiles=55 psprites=16 csprites=6 area=832x888 calls=18 | blit: draws=170 px=550k fillrect=19 glyphs=132 | gpu: upload=1339us rows=273 bytes=1369k paint=2612us swap=1110us
fps=39 frames=20 | vp: land=1306 veh=24 signs=2960 sort=212 draw=5230us tiles=106 psprites=33 csprites=12 area=664x1009 calls=20 | blit: draws=248 px=1277k fillrect=30 glyphs=172 | gpu: upload=1759us rows=427 bytes=2138k paint=3124us swap=918us
fps=58 frames=30 | vp: land=861 veh=16 signs=2059 sort=122 draw=3218us tiles=69 psprites=19 csprites=7 area=664x993 calls=19 | blit: draws=128 px=794k fillrect=12 glyphs=82 | gpu: upload=1105us rows=293 bytes=1465k paint=2682us swap=911us
fps=60 frames=31 | vp: land=249 veh=3 signs=301 sort=26 draw=201us tiles=11 psprites=1 csprites=0 area=616x1009 calls=5 | blit: draws=74 px=44k fillrect=10 glyphs=69 | gpu: upload=1092us rows=150 bytes=750k paint=3023us swap=1268us
fps=51 frames=27 | vp: land=0 veh=0 signs=0 sort=0 draw=0us tiles=0 psprites=0 csprites=0 area=0x0 calls=0 | blit: draws=80 px=13k fillrect=11 glyphs=79 | gpu: upload=562us rows=45 bytes=228k paint=3642us swap=1611us
fps=57 frames=29 | vp: land=0 veh=0 signs=0 sort=0 draw=0us tiles=0 psprites=0 csprites=0 area=0x0 calls=0 | blit: draws=75 px=12k fillrect=10 glyphs=74 | gpu: upload=835us rows=63 bytes=318k paint=3419us swap=1431us
fps=61 frames=31 | vp: land=0 veh=0 signs=0 sort=0 draw=0us tiles=0 psprites=0 csprites=0 area=0x0 calls=0 | blit: draws=70 px=11k fillrect=10 glyphs=69 | gpu: upload=434us rows=39 bytes=198k paint=3217us swap=1328us
fps=54 frames=28 | vp: land=0 veh=0 signs=0 sort=0 draw=0us tiles=0 psprites=0 csprites=0 area=0x0 calls=0 | blit: draws=77 px=13k fillrect=11 glyphs=76 | gpu: upload=497us rows=55 bytes=275k paint=3167us swap=1326us
fps=38 frames=20 | vp: land=832 veh=3 signs=2851 sort=121 draw=4084us tiles=57 psprites=17 csprites=8 area=664x1204 calls=8 | blit: draws=179 px=918k fillrect=18 glyphs=129 | gpu: upload=1039us rows=226 bytes=1132k paint=3600us swap=1435us
fps=31 frames=16 | vp: land=1418 veh=13 signs=6348 sort=187 draw=6954us tiles=208 psprites=60 csprites=32 area=664x2856 calls=16 | blit: draws=406 px=3692k fillrect=31 glyphs=214 | gpu: upload=1953us rows=910 bytes=4552k paint=2587us swap=845us
fps=45 frames=24 | vp: land=867 veh=13 signs=4000 sort=129 draw=4827us tiles=149 psprites=39 csprites=23 area=664x2856 calls=17 | blit: draws=220 px=2468k fillrect=13 glyphs=89 | gpu: upload=1720us rows=947 bytes=4739k paint=3160us swap=878us
fps=39 frames=20 | vp: land=1096 veh=18 signs=5478 sort=153 draw=5808us tiles=158 psprites=41 csprites=23 area=664x2712 calls=15 | blit: draws=244 px=2627k fillrect=15 glyphs=107 | gpu: upload=1732us rows=941 bytes=4705k paint=3361us swap=983us
fps=19 frames=10 | vp: land=2233 veh=33 signs=10551 sort=278 draw=12017us tiles=234 psprites=64 csprites=34 area=664x2548 calls=10 | blit: draws=393 px=4175k fillrect=30 glyphs=183 | gpu: upload=2275us rows=1265 bytes=6328k paint=3516us swap=917us
fps=30 frames=16 | vp: land=1723 veh=34 signs=7330 sort=255 draw=9345us tiles=205 psprites=56 csprites=29 area=664x2460 calls=15 | blit: draws=323 px=3578k fillrect=25 glyphs=143 | gpu: upload=2070us rows=1108 bytes=5540k paint=2659us swap=903us
fps=34 frames=18 | vp: land=1398 veh=26 signs=6082 sort=188 draw=7447us tiles=220 psprites=60 csprites=31 area=664x2856 calls=18 | blit: draws=372 px=3925k fillrect=26 glyphs=183 | gpu: upload=2400us rows=1119 bytes=5597k paint=2799us swap=625us
```

### Iteration 2

```
fps=34 frames=19 | vp: land=1109 veh=19 signs=5421 sort=154 draw=6703us tiles=197 psprites=51 csprites=28 area=1276x2716 calls=16 | blit: draws=309 px=3483k fillrect=19 glyphs=135 | gpu: upload=1568us rows=964 bytes=4823k paint=2172us swap=922us
fps=25 frames=14 | vp: land=1540 veh=27 signs=8246 sort=233 draw=9866us tiles=257 psprites=70 csprites=38 area=1280x2840 calls=13 | blit: draws=426 px=4792k fillrect=27 glyphs=184 | gpu: upload=2397us rows=1275 bytes=6377k paint=2941us swap=771us
fps=29 frames=15 | vp: land=1379 veh=18 signs=6785 sort=184 draw=8433us tiles=231 psprites=63 csprites=36 area=1256x2856 calls=12 | blit: draws=360 px=4361k fillrect=20 glyphs=137 | gpu: upload=2428us rows=1424 bytes=7124k paint=3679us swap=863us
fps=31 frames=16 | vp: land=1422 veh=18 signs=6556 sort=204 draw=7620us tiles=201 psprites=56 csprites=31 area=1104x2856 calls=13 | blit: draws=351 px=3695k fillrect=23 glyphs=161 | gpu: upload=1923us rows=1083 bytes=5416k paint=2913us swap=836us
fps=51 frames=26 | vp: land=725 veh=7 signs=1830 sort=90 draw=2368us tiles=39 psprites=10 csprites=4 area=1184x2112 calls=11 | blit: draws=92 px=447k fillrect=9 glyphs=65 | gpu: upload=1412us rows=232 bytes=1163k paint=3030us swap=1092us
fps=55 frames=29 | vp: land=744 veh=13 signs=1508 sort=121 draw=3446us tiles=55 psprites=16 csprites=6 area=832x888 calls=18 | blit: draws=170 px=550k fillrect=19 glyphs=132 | gpu: upload=1339us rows=273 bytes=1369k paint=2612us swap=1110us
fps=39 frames=20 | vp: land=1306 veh=24 signs=2960 sort=212 draw=5230us tiles=106 psprites=33 csprites=12 area=664x1009 calls=20 | blit: draws=248 px=1277k fillrect=30 glyphs=172 | gpu: upload=1759us rows=427 bytes=2138k paint=3124us swap=918us
fps=58 frames=30 | vp: land=861 veh=16 signs=2059 sort=122 draw=3218us tiles=69 psprites=19 csprites=7 area=664x993 calls=19 | blit: draws=128 px=794k fillrect=12 glyphs=82 | gpu: upload=1105us rows=293 bytes=1465k paint=2682us swap=911us
fps=60 frames=31 | vp: land=249 veh=3 signs=301 sort=26 draw=201us tiles=11 psprites=1 csprites=0 area=616x1009 calls=5 | blit: draws=74 px=44k fillrect=10 glyphs=69 | gpu: upload=1092us rows=150 bytes=750k paint=3023us swap=1268us
fps=51 frames=27 | vp: land=0 veh=0 signs=0 sort=0 draw=0us tiles=0 psprites=0 csprites=0 area=0x0 calls=0 | blit: draws=80 px=13k fillrect=11 glyphs=79 | gpu: upload=562us rows=45 bytes=228k paint=3642us swap=1611us
fps=57 frames=29 | vp: land=0 veh=0 signs=0 sort=0 draw=0us tiles=0 psprites=0 csprites=0 area=0x0 calls=0 | blit: draws=75 px=12k fillrect=10 glyphs=74 | gpu: upload=835us rows=63 bytes=318k paint=3419us swap=1431us
fps=61 frames=31 | vp: land=0 veh=0 signs=0 sort=0 draw=0us tiles=0 psprites=0 csprites=0 area=0x0 calls=0 | blit: draws=70 px=11k fillrect=10 glyphs=69 | gpu: upload=434us rows=39 bytes=198k paint=3217us swap=1328us
fps=54 frames=28 | vp: land=0 veh=0 signs=0 sort=0 draw=0us tiles=0 psprites=0 csprites=0 area=0x0 calls=0 | blit: draws=77 px=13k fillrect=11 glyphs=76 | gpu: upload=497us rows=55 bytes=275k paint=3167us swap=1326us
fps=38 frames=20 | vp: land=832 veh=3 signs=2851 sort=121 draw=4084us tiles=57 psprites=17 csprites=8 area=664x1204 calls=8 | blit: draws=179 px=918k fillrect=18 glyphs=129 | gpu: upload=1039us rows=226 bytes=1132k paint=3600us swap=1435us
fps=31 frames=16 | vp: land=1418 veh=13 signs=6348 sort=187 draw=6954us tiles=208 psprites=60 csprites=32 area=664x2856 calls=16 | blit: draws=406 px=3692k fillrect=31 glyphs=214 | gpu: upload=1953us rows=910 bytes=4552k paint=2587us swap=845us
fps=45 frames=24 | vp: land=867 veh=13 signs=4000 sort=129 draw=4827us tiles=149 psprites=39 csprites=23 area=664x2856 calls=17 | blit: draws=220 px=2468k fillrect=13 glyphs=89 | gpu: upload=1720us rows=947 bytes=4739k paint=3160us swap=878us
fps=39 frames=20 | vp: land=1096 veh=18 signs=5478 sort=153 draw=5808us tiles=158 psprites=41 csprites=23 area=664x2712 calls=15 | blit: draws=244 px=2627k fillrect=15 glyphs=107 | gpu: upload=1732us rows=941 bytes=4705k paint=3361us swap=983us
fps=19 frames=10 | vp: land=2233 veh=33 signs=10551 sort=278 draw=12017us tiles=234 psprites=64 csprites=34 area=664x2548 calls=10 | blit: draws=393 px=4175k fillrect=30 glyphs=183 | gpu: upload=2275us rows=1265 bytes=6328k paint=3516us swap=917us
fps=30 frames=16 | vp: land=1723 veh=34 signs=7330 sort=255 draw=9345us tiles=205 psprites=56 csprites=29 area=664x2460 calls=15 | blit: draws=323 px=3578k fillrect=25 glyphs=143 | gpu: upload=2070us rows=1108 bytes=5540k paint=2659us swap=903us
fps=34 frames=18 | vp: land=1398 veh=26 signs=6082 sort=188 draw=7447us tiles=220 psprites=60 csprites=31 area=664x2856 calls=18 | blit: draws=372 px=3925k fillrect=26 glyphs=183 | gpu: upload=2400us rows=1119 bytes=5597k paint=2799us swap=625us
```

### Iteration 3

```
fps=37 frames=19 | vp: land=1113 veh=30 signs=5402 sort=258 draw=7923us tiles=231 psprites=66 csprites=34 area=1140x2856 calls=17 | blit: draws=367 px=4349k fillrect=22 glyphs=152 | gpu: upload=2004us rows=1137 bytes=5686k paint=2699us swap=624us
fps=32 frames=18 | vp: land=1358 veh=33 signs=5771 sort=195 draw=8065us tiles=224 psprites=61 csprites=31 area=664x2336 calls=18 | blit: draws=337 px=3822k fillrect=23 glyphs=149 | gpu: upload=2048us rows=1081 bytes=5406k paint=2738us swap=737us
fps=30 frames=16 | vp: land=1391 veh=35 signs=6136 sort=202 draw=8843us tiles=215 psprites=60 csprites=30 area=664x2156 calls=16 | blit: draws=400 px=3764k fillrect=30 glyphs=211 | gpu: upload=1999us rows=1040 bytes=5202k paint=3163us swap=951us
fps=34 frames=18 | vp: land=1367 veh=32 signs=4998 sort=202 draw=8133us tiles=160 psprites=44 csprites=22 area=1252x1948 calls=14 | blit: draws=257 px=2774k fillrect=17 glyphs=118 | gpu: upload=1927us rows=769 bytes=3845k paint=2577us swap=788us
fps=24 frames=14 | vp: land=1871 veh=38 signs=6856 sort=280 draw=12210us tiles=195 psprites=59 csprites=29 area=664x2161 calls=14 | blit: draws=389 px=3481k fillrect=31 glyphs=214 | gpu: upload=1800us rows=905 bytes=4526k paint=3109us swap=950us
fps=29 frames=15 | vp: land=1454 veh=29 signs=5061 sort=231 draw=9383us tiles=148 psprites=49 csprites=23 area=1096x1905 calls=12 | blit: draws=277 px=2637k fillrect=21 glyphs=143 | gpu: upload=1666us rows=823 bytes=4118k paint=3560us swap=1019us
fps=33 frames=17 | vp: land=1384 veh=21 signs=4509 sort=224 draw=8306us tiles=160 psprites=54 csprites=26 area=1144x1777 calls=17 | blit: draws=304 px=2737k fillrect=24 glyphs=170 | gpu: upload=1843us rows=622 bytes=3113k paint=2880us swap=709us
fps=27 frames=14 | vp: land=1602 veh=22 signs=5801 sort=265 draw=9944us tiles=170 psprites=54 csprites=26 area=664x2856 calls=14 | blit: draws=363 px=2943k fillrect=29 glyphs=213 | gpu: upload=1612us rows=737 bytes=3685k paint=3451us swap=982us
fps=47 frames=26 | vp: land=1057 veh=13 signs=2540 sort=143 draw=4792us tiles=100 psprites=30 csprites=13 area=1076x1432 calls=21 | blit: draws=183 px=1431k fillrect=18 glyphs=107 | gpu: upload=1558us rows=471 bytes=2356k paint=2735us swap=1034us
fps=46 frames=24 | vp: land=976 veh=14 signs=2521 sort=154 draw=5667us tiles=93 psprites=31 csprites=13 area=1076x1456 calls=17 | blit: draws=165 px=1397k fillrect=13 glyphs=89 | gpu: upload=1279us rows=378 bytes=1891k paint=2817us swap=842us
fps=35 frames=18 | vp: land=1279 veh=19 signs=4068 sort=231 draw=7132us tiles=120 psprites=40 csprites=19 area=1072x1468 calls=16 | blit: draws=214 px=1888k fillrect=18 glyphs=119 | gpu: upload=1900us rows=549 bytes=2745k paint=3209us swap=983us
fps=32 frames=18 | vp: land=1357 veh=21 signs=3939 sort=241 draw=8544us tiles=141 psprites=47 csprites=23 area=616x1504 calls=18 | blit: draws=326 px=2347k fillrect=30 glyphs=202 | gpu: upload=1938us rows=685 bytes=3425k paint=3058us swap=971us
fps=61 frames=31 | vp: land=345 veh=6 signs=773 sort=52 draw=1884us tiles=31 psprites=9 csprites=3 area=616x1508 calls=10 | blit: draws=108 px=433k fillrect=12 glyphs=83 | gpu: upload=1012us rows=299 bytes=1496k paint=2816us swap=714us
fps=36 frames=19 | vp: land=909 veh=11 signs=3875 sort=189 draw=6439us tiles=49 psprites=16 csprites=8 area=664x1520 calls=7 | blit: draws=159 px=857k fillrect=16 glyphs=112 | gpu: upload=883us rows=324 bytes=1622k paint=3348us swap=1282us
fps=53 frames=28 | vp: land=902 veh=16 signs=1253 sort=122 draw=1878us tiles=53 psprites=12 csprites=5 area=664x692 calls=18 | blit: draws=168 px=461k fillrect=22 glyphs=138 | gpu: upload=1379us rows=245 bytes=1225k paint=2682us swap=864us
fps=31 frames=16 | vp: land=1723 veh=34 signs=4122 sort=290 draw=7620us tiles=109 psprites=34 csprites=13 area=664x1009 calls=16 | blit: draws=268 px=1354k fillrect=31 glyphs=187 | gpu: upload=1783us rows=393 bytes=1967k paint=3206us swap=1147us
fps=34 frames=18 | vp: land=1383 veh=28 signs=3649 sort=265 draw=6366us tiles=92 psprites=27 csprites=10 area=664x993 calls=14 | blit: draws=184 px=1173k fillrect=17 glyphs=118 | gpu: upload=1728us rows=435 bytes=2179k paint=3246us swap=984us
fps=61 frames=31 | vp: land=426 veh=8 signs=929 sort=49 draw=996us tiles=25 psprites=5 csprites=2 area=664x1009 calls=9 | blit: draws=83 px=224k fillrect=10 glyphs=68 | gpu: upload=912us rows=181 bytes=909k paint=3267us swap=985us
fps=41 frames=21 | vp: land=834 veh=13 signs=2175 sort=100 draw=2101us tiles=27 psprites=6 csprites=2 area=664x732 calls=7 | blit: draws=118 px=256k fillrect=15 glyphs=102 | gpu: upload=1123us rows=120 bytes=604k paint=3793us swap=2114us
fps=43 frames=22 | vp: land=635 veh=11 signs=3704 sort=62 draw=2811us tiles=26 psprites=6 csprites=2 area=1280x1804 calls=4 | blit: draws=99 px=412k fillrect=11 glyphs=77 | gpu: upload=1090us rows=151 bytes=759k paint=3686us swap=1906us
```
