# GPU-Only Rendering Architecture for OpenTTD

Исследование и план перехода с CPU-рендеринга на полностью GPU-based rendering pipeline.

## Содержание

| Файл | Описание |
|------|----------|
| [01_current_rendering_pipeline.md](01_current_rendering_pipeline.md) | Полный flow текущего CPU-рендеринга: от GameTick до пикселей на экране. Call chain, структуры данных, где тратится CPU время. |
| [02_viewport_system.md](02_viewport_system.md) | Viewport: итерация тайлов, сбор спрайтов, сортировка, рисование. ViewportAddLandscape, ParentSpriteToDraw, dirty blocks. Главный bottleneck. |
| [03_sprite_and_blitter_system.md](03_sprite_and_blitter_system.md) | Система спрайтов: форматы (RGBA + M-channel), BlitterMode, SubSprite clipping, zoom levels, спрайт-кэш. Что нужно перенести на GPU. |
| [04_text_and_ui_rendering.md](04_text_and_ui_rendering.md) | Рендеринг текста (glyph sprites), UI виджетов (GfxFillRect, DrawFrameRect), линий. Всё — через CPU blitter. |
| [05_gpu_architecture_design.md](05_gpu_architecture_design.md) | Архитектура GPU-only рендеринга: retained scene graph, sprite atlas, command buffer, шейдеры для каждого BlitterMode. |
| [06_implementation_phases.md](06_implementation_phases.md) | Поэтапный план реализации: от текущего состояния до полного GPU рендеринга. Зависимости, риски, оценка сложности. |
| [07_performance_measurements.md](07_performance_measurements.md) | Замеры производительности: 3 итерации по 10 сек. FPS 19-61, bottleneck breakdown, raw данные всех счётчиков. |

## Текущее состояние

- CPU blitter рисует ВСЁ в pixel buffer (14MB @ 1280x2856)
- Buffer загружается в GPU текстуру каждый кадр
- GPU только показывает эту текстуру на экране
- FPS: 20-50, bottleneck = viewport redraw (tile iteration + sprite compositing)

## Целевое состояние

- GPU рисует все спрайты напрямую из атласа текстур
- CPU только определяет ЧТО рисовать (tile iteration, game logic)
- Retained scene graph кэширует спрайт-лист между кадрами
- Перерисовка только изменившихся элементов (vehicles, animations)
