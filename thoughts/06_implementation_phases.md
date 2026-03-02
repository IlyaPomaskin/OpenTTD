# Фазы реализации GPU рендеринга

## Фаза 0: Текущее состояние ✅

**Что сделано:**
- GLES backend с FBO pipeline
- Sprite atlas (colour + remap)
- Шейдеры: normal, palette, remap, transparent, solid, bgra
- GPU sprite draw command queue
- Partial texture upload (glTexSubImage2D)
- Dirty block coalescing
- Viewport throttling (every 2nd frame)

**Что работает:**
- Полный CPU рендеринг через GLES blitter → upload → FBO → экран
- FPS 20-50 на Android (bottleneck: CPU compositing)

**Что НЕ работает:**
- `_gles_gpu_sprites = true` даёт чёрный viewport (CPU buffer пуст для viewport sprites)
- GPU sprites работают только при одновременной CPU отрисовке (дублирование работы)

---

## Фаза 1: GPU viewport спрайты (замена CPU compositing)

**Цель:** viewport спрайты рисуются GPU, CPU buffer только для UI.

### 1.1 Перехват ViewportDrawParentSprites

Вместо вызова `DrawSpriteViewport()` → `Blitter::Draw()`,
переделать на `DrawSpriteViewport()` → `QueueDraw()`:

```cpp
// src/gfx.cpp — модифицировать DrawSpriteViewport()
void DrawSpriteViewport(SpriteID img, PaletteID pal, int x, int y, const SubSprite *sub)
{
    if (_gles_gpu_sprites && GLESBackend::Get() != nullptr) {
        // Проверить что рисуем в viewport (не в screenshot)
        if (is_viewport_dpi()) {
            QueueViewportSprite(img, pal, x, y, sub);
            return;
        }
    }
    // Fallback: CPU rendering (как сейчас)
    ...
}
```

### 1.2 Разделение FBO на слои

```
FBO rendering order:
1. Clear FBO
2. Draw CPU buffer (UI only) → fullscreen quad с bgra shader
3. Draw GPU viewport sprites → batched quads из atlas
4. Draw CPU buffer overlay (cursors, tooltips) → partial quad
```

Проблема: сейчас CPU buffer содержит ВСЁ (viewport + UI).
Нужно разделить viewport отрисовку от UI отрисовки.

### 1.3 Пустой viewport в CPU buffer

Когда `_gles_gpu_sprites = true`:
- `ViewportDrawParentSprites()` не рисует в CPU buffer (только QueueDraw)
- CPU buffer содержит UI (toolbar, windows, text)
- Viewport область в CPU buffer = чёрная (или прозрачная)

**Подход**: viewport background (ground colour) рисуется GPU,
viewport sprites рисуются GPU, UI рисуется CPU поверх.

### 1.4 Fixing current GPU sprites issues

Текущие проблемы с `_gles_gpu_sprites = true`:
- Draw queue очищается каждый кадр, но viewport redraws не каждый кадр
- **Решение**: persistent draw queue (не очищать, обновлять при viewport redraw)

```cpp
class GLESBackend {
    std::vector<GLESDrawCommand> viewport_commands;  // Persistent viewport sprites
    std::vector<GLESDrawCommand> ui_commands;         // Per-frame UI sprites

    void BeginViewportDraw() { viewport_commands.clear(); }
    void QueueViewportDraw(cmd) { viewport_commands.push_back(cmd); }
    void QueueUIDraw(cmd) { ui_commands.push_back(cmd); }

    void Paint() {
        // 1. Draw viewport sprites (persistent, updated only on viewport redraw)
        RenderCommands(viewport_commands);
        // 2. Upload CPU buffer for UI
        UploadCPUBuffer();
        // 3. Draw CPU buffer overlay
        RenderCPUOverlay();
        // 4. Draw UI sprites
        RenderCommands(ui_commands);
        ui_commands.clear();
    }
};
```

### Ожидаемый результат

- Viewport: GPU рисует ~60-100 спрайтов из atlas → ~1-3 ms
- Upload: только UI часть (маленькая) → ~1-2 ms
- Итого: экономия 5-15 ms → FPS 40-60 стабильно

### Файлы для изменения

| Файл | Изменение |
|------|-----------|
| `src/gfx.cpp` | DrawSpriteViewport → GPU path для viewport |
| `src/viewport.cpp` | BeginViewportDraw/EndViewportDraw hooks |
| `src/video/gles_backend.h` | Persistent viewport commands |
| `src/video/gles_backend.cpp` | Split rendering: viewport → UI overlay |
| `src/video/sdl2_gles_v.cpp` | Порядок Paint: viewport → UI → overlay |

### Риски

- **SubSprite clipping** — нужна UV-based clipping для foundations
- **Remap table** — для каждого спрайта может быть своя remap table (company colours)
  - Решение: загружать remap table как текстуру, индексировать по remap_idx
- **Missing sprites in atlas** — CPU fallback для спрайтов не в атласе

---

## Фаза 2: GPU UI рендеринг

**Цель:** UI тоже рисуется GPU, CPU buffer полностью не нужен.

### 2.1 Перехват GfxFillRect

```cpp
void GfxFillRect(left, top, right, bottom, colour, mode)
{
    if (_gles_gpu_sprites && is_screen_dpi()) {
        switch (mode) {
            case FILLRECT_OPAQUE:
                QueueSolidRect(left, top, right-left, bottom-top, colour);
                return;
            case FILLRECT_CHECKER:
                QueueCheckerRect(left, top, right-left, bottom-top, colour);
                return;
            case FILLRECT_RECOLOUR:
                QueueRecolourRect(left, top, right-left, bottom-top, palette_id);
                return;
        }
    }
    // Fallback
    ...
}
```

### 2.2 Перехват DrawSprite (UI)

```cpp
void DrawSprite(SpriteID img, PaletteID pal, int x, int y, const SubSprite *sub, ZoomLevel zoom)
{
    if (_gles_gpu_sprites && is_screen_dpi()) {
        QueueUIDraw(img, pal, x, y, sub, zoom);
        return;
    }
    // Fallback
    ...
}
```

### 2.3 Перехват DrawString

Текст = серия glyph sprites. Перехват на уровне `GfxMainBlitter()`:
```cpp
static void GfxMainBlitter(const Sprite *sprite, int x, int y, BlitterMode mode, ...)
{
    if (_gles_gpu_sprites && is_screen_dpi()) {
        QueueUIDraw(sprite_id, x, y, mode, ...);
        return;
    }
    // Fallback
    ...
}
```

### 2.4 Порядок рисования

```
Paint():
  1. Clear FBO
  2. Render viewport sprites (from retained scene graph or persistent commands)
  3. Render UI: solid rects → sprite quads → text glyphs
  4. Render overlay: cursors, tooltips
  5. Blit FBO → screen
```

### Файлы для изменения

| Файл | Изменение |
|------|-----------|
| `src/gfx.cpp` | GfxFillRect, DrawSprite, GfxMainBlitter → GPU path |
| `src/gfx.cpp` | GfxDrawLine → GPU thin quad |
| `src/video/gles_backend.cpp` | Новые шейдеры (checker, recolour, line) |
| `src/table/gles_shader.h` | GLSL код новых шейдеров |

### Ожидаемый результат

- **Нет CPU buffer upload** вообще → экономия 4-6 ms
- **Нет CPU pixel compositing** → экономия всех Blitter::Draw() вызовов
- FPS limited by: tile iteration (1-6 ms) + GPU rendering (1-3 ms) = 60+ FPS

---

## Фаза 3: Retained Scene Graph

**Цель:** не итерировать тайлы каждый кадр.

### 3.1 Scene graph структура

```cpp
struct SceneSprite {
    GLESSpriteID key;
    int16_t x, y;
    int16_t w, h;
    int16_t skip_x, skip_y;
    BlitterMode mode;
    uint8_t remap_idx;
    TileIndex source_tile;  // Для инвалидации
};

class ViewportSceneGraph {
    std::vector<SceneSprite> sprites;
    std::unordered_map<TileIndex, std::pair<size_t, size_t>> tile_ranges;
    bool needs_full_rebuild = true;
    Rect viewport_rect;
    ZoomLevel zoom;

    void FullRebuild();        // Полная перестройка (scroll, zoom)
    void InvalidateTile(TileIndex);  // Пометить тайл грязным
    void UpdateDirtyTiles();   // Перестроить только грязные тайлы
    void SubmitToGPU();        // Отправить все commands на GPU
};
```

### 3.2 Перехват dirty tile events

```cpp
// Модифицировать MarkTileDirtyByTile()
void MarkTileDirtyByTile(TileIndex tile, ...)
{
    if (scene_graph != nullptr) {
        scene_graph->InvalidateTile(tile);
    }
    // Стандартный dirty block path
    ...
}
```

### 3.3 Vehicle tracking

Транспорт обновляется отдельно от ландшафта:
```cpp
// Транспорт не привязан к тайлам, обновляется через Vehicle::UpdatePosition()
// Решение: vehicle sprites — отдельный слой в scene graph

class ViewportSceneGraph {
    std::vector<SceneSprite> landscape_sprites;  // Статичные тайлы
    std::vector<SceneSprite> vehicle_sprites;    // Обновляются каждый кадр
};
```

### 3.4 Scroll обработка

При скролле viewport:
1. Shift все позиции: `for (auto &s : sprites) { s.x -= dx; s.y -= dy; }`
2. Удалить спрайты за пределами экрана
3. Итерировать ТОЛЬКО новые тайлы на краях
4. Вставить их спрайты в scene graph

### Ожидаемый результат

- **Без скролла, без изменений**: 0 ms tile iteration → GPU only → 60+ FPS
- **С анимацией (vehicles, water)**: ~0.5 ms vehicle update + ~1 ms GPU → 60+ FPS
- **При скролле**: ~1-2 ms edge tiles + shift + GPU → 45+ FPS

---

## Фаза 4: Оптимизации

### 4.1 Instanced rendering
Одинаковые спрайты (трава, рельсы) → один draw call с instancing.
GLES 2.0 не поддерживает instancing, но GLES 3.0 да (`glDrawArraysInstanced`).

### 4.2 Texture array atlas
Вместо отдельных текстур — texture array (GLES 3.0).
Все zoom levels в одном массиве текстур → меньше bind calls.

### 4.3 Double-buffered scene graph
Два scene graph: один рисуется GPU, другой обновляется CPU.
Swap каждый кадр → zero-copy update.

### 4.4 Palette animation в шейдере
Cycling palette indices (227-254): shader автоматически сдвигает индексы
на основе uniform `u_palette_offset`, без перезагрузки текстуры палитры.

---

## Оценка сложности

| Фаза | Файлов | Изменений | Риск | Результат |
|------|--------|-----------|------|-----------|
| Фаза 1 | ~5 | Средний | Средний | 40-60 FPS стабильно |
| Фаза 2 | ~4 | Средний | Низкий | 60+ FPS, нет CPU buffer |
| Фаза 3 | ~3 | Большой | Высокий | 60 FPS при любых условиях |
| Фаза 4 | ~3 | Малый | Низкий | Marginal improvements |

**Рекомендация:** Фаза 1 даёт наибольший ROI. Фаза 3 — самая сложная но самая мощная.
