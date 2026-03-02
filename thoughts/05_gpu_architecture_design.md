# GPU-Only Architecture Design

## Обзор

Цель: устранить CPU pixel compositing. CPU определяет ЧТО рисовать,
GPU рисует всё напрямую из sprite atlas.

### Текущий flow (CPU)
```
Tile iteration → Sprite list → CPU pixel compositing → Upload buffer → GPU display
   1-6 ms           ~0 ms          1-17 ms              4-6 ms          ~1 ms
```

### Целевой flow (GPU)
```
Tile iteration → GPU command list → GPU batch render from atlas
   1-6 ms           ~0 ms              1-3 ms
```

Экономия: **5-23 ms на кадр** (нет CPU compositing + нет upload).

## Архитектура: 3 уровня

### Level 1: GPU Sprite Rendering (immediate mode)

Каждый кадр: tile iteration → sprite list → GPU draw commands → GPU render.

**Принцип**: заменить `Blitter::Draw()` на `GLESBackend::QueueDraw()` для ВСЕХ операций,
не только viewport спрайтов.

```
ViewportDoDraw()
  ├── ViewportAddLandscape() → AddSortableSpriteToDraw() → parent_sprites_to_draw
  ├── ViewportAddVehicles()  → AddSortableSpriteToDraw()
  ├── Sort sprites by Z
  └── ViewportDrawParentSprites()
      └── DrawSpriteViewport()
          └── GfxMainBlitterViewport()
              └── [ВМЕСТО Blitter::Draw()] → QueueDraw(sprite_key, x, y, mode, remap)

Window::DrawWidgets()
  ├── DrawFrameRect() → GfxFillRect() → [ВМЕСТО DrawRect()] → QueueSolid(x, y, w, h, colour)
  ├── DrawString()    → per glyph → [ВМЕСТО Draw()] → QueueDraw(glyph_key, x, y, remap)
  └── DrawSprite()    → [ВМЕСТО Draw()] → QueueDraw(sprite_key, x, y, mode)
```

**CPU buffer полностью не нужен.** Все рисуется в FBO через GPU.

### Level 2: Retained Scene Graph (diff mode)

Кэш спрайт-листа между кадрами. Обновлять только изменившиеся элементы.

```
Frame N:   Full tile iteration → scene_graph = [sprite1, sprite2, ..., spriteN]
Frame N+1: Dirty tiles only   → update sprite3, sprite7 in scene_graph
Frame N+2: No changes         → reuse scene_graph as-is
```

**Структура scene graph:**
```cpp
struct SceneEntry {
    GLESSpriteID sprite_key;
    int16_t screen_x, screen_y;
    int16_t width, height;
    BlitterMode mode;
    uint8_t remap_idx;
    ZoomLevel zoom;
    uint32_t tile_id;     // Какому тайлу принадлежит (для инвалидации)
    uint32_t layer;       // Z-order слой
};

class RetainedSceneGraph {
    std::vector<SceneEntry> entries;
    std::unordered_map<TileIndex, Range> tile_entries;  // tile → диапазон entries
    bool full_rebuild_needed;
    std::unordered_set<TileIndex> dirty_tiles;

    void MarkTileDirty(TileIndex tile);
    void RebuildTile(TileIndex tile);    // Перестроить entries для одного тайла
    void RebuildFull();                   // Полная перестройка (scroll, zoom change)
    void Render();                        // Submit все entries как GPU draw commands
};
```

### Level 3: Incremental Viewport Updates

При скролле: shift все позиции, добавить/удалить крайние тайлы.
При zoom: полная перестройка (другие спрайты).

```
Scroll dx,dy:
  1. Для всех entries: screen_x -= dx, screen_y -= dy
  2. Удалить entries за пределами экрана
  3. Добавить entries для новых видимых тайлов на краях
```

## GPU Command Buffer

### Unified command type

```cpp
enum class GPUDrawType : uint8_t {
    Sprite,       // Спрайт из атласа (viewport, UI icons, glyph)
    SolidRect,    // Сплошной прямоугольник (UI backgrounds)
    RemapRect,    // Ремап прямоугольник (FILLRECT_RECOLOUR)
    Line,         // Линия
};

struct GPUDrawCommand {
    GPUDrawType type;
    BlitterMode mode;

    // Общие поля
    int16_t x, y, width, height;

    // Для спрайтов
    GLESSpriteID sprite_key;
    int16_t skip_left, skip_top;
    int16_t sprite_width, sprite_height;
    ZoomLevel zoom;
    uint8_t remap_idx;

    // Для прямоугольников
    uint32_t colour;  // RGBA
};
```

### Batching strategy

Команды сортируются для минимизации state changes:
1. По шейдеру (normal → palette → remap → transparent → solid)
2. По текстуре атласа (minimize glBindTexture)
3. Внутри группы — по Z-order (original draw order)

### Vertex format

Текущий формат достаточен:
```cpp
struct GLESVertex {
    float x, y;     // Screen position
    float u, v;     // Colour atlas UV
    float ru, rv;   // Remap atlas UV
};
```

Для solid rects: UV не используется.
Для text: UV = glyph в atlas, ru/rv = не используется (или remap UV для цвета).

## Шейдеры — полный набор для GPU-only

### Уже реализованы
- `prog_normal` — RGBA спрайты с alpha blend
- `prog_palette` — M → palette lookup
- `prog_remap` — M → remap_table → palette (company colours)
- `prog_transparent` — Затемнение фона
- `prog_solid` — Сплошной цвет
- `prog_bgra` — CPU buffer (не нужен в GPU-only)

### Нужно добавить
- `prog_crash_remap` — RGB → grayscale → palette (для CrashRemap)
- `prog_black_remap` — Всё чёрным (для BlackRemap)
- `prog_checker` — Шахматный паттерн (для FILLRECT_CHECKER)
- `prog_recolour_rect` — Палитрный ремап для прямоугольника

### Пример нового шейдера для FILLRECT_CHECKER

```glsl
precision mediump float;
uniform vec4 u_colour;
void main() {
    // Шахматный паттерн: discard каждый второй пиксель
    if (mod(floor(gl_FragCoord.x) + floor(gl_FragCoord.y), 2.0) < 0.5) discard;
    gl_FragColor = u_colour;
}
```

## Обработка SubSprite (clipping)

**Текущий подход (CPU)**: GfxBlitter вычисляет skip_left/skip_top и обрезает width/height.

**GPU подход**: два варианта:

### Вариант A: UV clipping (проще)
Вместо рисования полного спрайта, сдвигаем UV координаты:
```
u0 = entry.u0 + (skip_left / sprite_width) * (entry.u1 - entry.u0)
v0 = entry.v0 + (skip_top / sprite_height) * (entry.v1 - entry.v0)
u1 = u0 + (visible_width / sprite_width) * (entry.u1 - entry.u0)
v1 = v0 + (visible_height / sprite_height) * (entry.v1 - entry.v0)
```

### Вариант B: Scissor test (медленнее, но точнее)
```cpp
glEnable(GL_SCISSOR_TEST);
glScissor(sub->left, screen_height - sub->bottom, sub->right - sub->left, sub->bottom - sub->top);
// Draw sprite
glDisable(GL_SCISSOR_TEST);
```

**Рекомендация**: Вариант A (UV clipping) — не требует state change, работает в batch.

## Порядок рисования (Z-order)

GPU рисует в порядке submit'а команд. Для корректного перекрытия:

1. **Background** — ground tiles (самый дальний слой)
2. **Buildings/terrain** — отсортированные по isometric Z-order
3. **Vehicles** — вперемешку с buildings по Z-order
4. **UI Windows** — поверх viewport, в window Z-order
5. **Tooltips/Cursors** — самый верхний слой

Сортировка уже выполняется в `ViewportDoDraw()` → `_vp_sprite_sorter()`.
GPU просто рисует в том же порядке.

## Устранение CPU buffer

В GPU-only архитектуре:
- `_screen.dst_ptr` → **не нужен** для рисования, но нужен для:
  - Screenshots (SaveScreenshot)
  - Blitter fallback (спрайты не в атласе)
  - GfxScroll (scroll optimization — можно заменить FBO blit)

Решение: сохранить buffer для compatibility, но не использовать для рендеринга.
Для screenshots: `glReadPixels()` из FBO.
