# Текущий Rendering Pipeline (CPU)

## Полный call chain: от GameTick до пикселей

```
GameTick()                          // Обновление игровой логики
  ├── Vehicle::Tick()               // Движение транспорта
  │   └── Vehicle::UpdatePosition()
  │       └── MarkAllViewportsDirty()  // Помечает dirty blocks для области транспорта
  ├── AnimateTile()                 // Анимация тайлов (вода, маяки, etc.)
  │   └── MarkTileDirtyByTile()
  └── ...

UpdateWindows(delta_ms)             // src/window.cpp:3200
  ├── DrawDirtyBlocks()             // src/gfx.cpp:1458 — основной entry point рендеринга
  │   ├── [coalesced path]          // Собирает все dirty blocks в один прямоугольник
  │   └── RedrawScreenRect()        // Перерисовывает область экрана
  │       └── Window::DrawViewport()
  │           └── ViewportDoDraw()  // src/viewport.cpp:1810 — ГЛАВНАЯ функция
  │               ├── ViewportAddLandscape()   // Итерация тайлов → спрайт-лист
  │               ├── ViewportAddVehicles()    // Добавление транспорта
  │               ├── ViewportAddKdtreeSigns() // Указатели, названия городов
  │               ├── DrawTextEffects()        // Плавающие числа
  │               ├── ViewportDrawTileSprites()// Рисование ground спрайтов
  │               ├── _vp_sprite_sorter()      // Сортировка по Z-order
  │               └── ViewportDrawParentSprites() // Рисование всех спрайтов
  │                   └── DrawSpriteViewport() // src/gfx.cpp:1012
  │                       └── GfxMainBlitterViewport()
  │                           └── GfxBlitter<ZOOM_BASE, false>()
  │                               └── Blitter::Draw(bp, mode, zoom)  // ПИКСЕЛЬНАЯ работа
  └── DrawMouseCursor()

Paint()                             // src/video/sdl2_gles_v.cpp:167
  ├── UploadVideoBuffer()           // glTexSubImage2D — загрузка CPU buffer в GPU
  ├── GLESBackend::Paint()          // Отрисовка текстуры на экран через FBO
  └── SDL_GL_SwapWindow()           // Показ кадра
```

## Где тратится время (из профиля)

Типичные значения на Android (1280x2856 экран):

| Фаза | Время | Что делает |
|------|-------|------------|
| `ViewportAddLandscape` | 1-6 ms | Итерация всех видимых тайлов, вызов draw_tile_proc |
| `signs+tiles` | 2-15 ms | DrawTileSprites + KdtreeSigns + TextEffects |
| `sort` | 0.1-0.5 ms | Сортировка спрайтов по Z-order (SSE41 оптимизирована) |
| `ViewportDrawParentSprites` | 1-17 ms | **Рисование каждого спрайта через Blitter::Draw()** |
| `UploadVideoBuffer` | 4-6 ms | glTexSubImage2D (partial rows) |
| `GLESBackend::Paint` | 1-2 ms | GPU рендеринг FBO → экран |

**Bottleneck**: `ViewportDrawParentSprites` — CPU попиксельно композитит каждый спрайт в buffer.

## DrawPixelInfo — ключевая структура

```cpp
// src/gfx_type.h
struct DrawPixelInfo {
    void *dst_ptr;    // Указатель на pixel buffer (куда рисуем)
    int left, top;    // Offset рисования
    int width, height;// Размер области
    int pitch;        // Строка в пикселях
    ZoomLevel zoom;   // Текущий уровень зума
};
```

Вся система рендеринга работает через `_cur_dpi` — глобальный указатель на текущий DrawPixelInfo.
Viewport устанавливает `_cur_dpi` на подобласть screen buffer перед отрисовкой.

## Blitter::BlitterParams — параметры для отрисовки одного спрайта

```cpp
// src/blitter/base.hpp
struct BlitterParams {
    const void *sprite;         // Сырые данные спрайта (SpriteData для 32bpp)
    const uint8_t *remap;       // Таблица ремапа палитры (256 байт)
    int skip_left, skip_top;    // Смещение в исходном спрайте (clipping)
    int width, height;          // Размер видимой области
    int sprite_width, sprite_height; // Полный размер спрайта
    int left, top;              // Позиция в destination buffer
    void *dst;                  // Указатель на destination buffer
    int pitch;                  // Pitch destination buffer
};
```

## Dirty Blocks — механизм инвалидации

- Экран разделён на блоки 64x8 пикселей
- При изменении (транспорт двинулся, анимация тайла) → `AddDirtyBlock()`
- `DrawDirtyBlocks()` собирает грязные блоки и вызывает `RedrawScreenRect()` для каждой области
- В GLES режиме: все блоки коалесцируются в один прямоугольник → один `RedrawScreenRect()`
- Проблема: разбросанные анимации → огромный bounding rect → перерисовка почти всего экрана

## Ключевые файлы

| Файл | Роль |
|------|------|
| `src/gfx.cpp` | DrawDirtyBlocks, GfxBlitter, GfxFillRect, DrawSpriteViewport |
| `src/viewport.cpp` | ViewportDoDraw, ViewportAddLandscape, ViewportDrawParentSprites |
| `src/blitter/gles.cpp` | GLES блиттер (CPU fallback + GPU command queue) |
| `src/video/sdl2_gles_v.cpp` | Video driver: Paint(), UploadVideoBuffer() |
| `src/video/gles_backend.cpp` | GPU backend: шейдеры, FBO, batch rendering |
