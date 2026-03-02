# Viewport System — Главный Bottleneck

## ViewportAddLandscape — итерация тайлов

Файл: `src/viewport.cpp:1206-1316`

### Алгоритм

1. Преобразует экранные координаты dirty rect → тайловые координаты (isometric)
2. Итерирует по строкам и столбцам тайлов в видимой области
3. Для каждого видимого тайла вызывает `_tile_type_procs[tile_type]->draw_tile_proc(&_cur_ti)`
4. `draw_tile_proc` добавляет спрайты через `AddSortableSpriteToDraw()`

```cpp
// src/viewport.cpp:1238-1315 (упрощённо)
for (row = start_row; !last_row; row++) {
    for (column = left_column; column <= right_column; column++) {
        // Преобразование row/column → tile координаты
        TileType tile_type = GetTileType(tile);

        // Каждый тайл вызывает свой draw proc
        if (tile_visible) {
            _tile_type_procs[tile_type]->draw_tile_proc(&_cur_ti);
        }
    }
}
```

### Tile Type Procs — что каждый тип тайла делает

Каждый тип тайла (ландшафт, дорога, рельсы, вода, здания...) имеет свой `draw_tile_proc`.
Эти функции вызывают `DrawGroundSprite()` и `AddSortableSpriteToDraw()` для добавления
спрайтов в список рисования.

```
TileType::Clear    → DrawClearLandTile()    // Трава, пустыня, снег
TileType::Rail     → DrawTrackBits()        // Рельсы
TileType::Road     → DrawRoadBits()         // Дороги
TileType::Town     → DrawTownTile()         // Здания
TileType::Trees    → DrawTreeTile()         // Деревья
TileType::Station  → DrawStationTile()      // Станции
TileType::Water    → DrawWaterTile()        // Вода (анимированная!)
TileType::Industry → DrawIndustryTile()     // Фабрики
TileType::Object   → DrawObjectTile()       // Маяки и др. объекты
```

### Стоимость итерации

На экране 1280x2856 при нормальном зуме видно ~200-400 тайлов.
Для каждого тайла:
- `GetTileType()` — чтение из карты (быстро, cache-friendly)
- `GetTilePixelSlope()` — высота и наклон
- `draw_tile_proc()` — 1-10 вызовов `AddSortableSpriteToDraw()` (в зависимости от сложности тайла)
- Каждый `AddSortableSpriteToDraw()` делает `GetSprite()` для расчёта bounding box

**Итого**: ~1-6 ms на итерацию тайлов.

## AddSortableSpriteToDraw — запись спрайта в список

Файл: `src/viewport.cpp:658-730`

```cpp
void AddSortableSpriteToDraw(SpriteID image, PaletteID pal,
                              int x, int y, int z,
                              const SpriteBounds &bounds,
                              bool transparent, const SubSprite *sub)
{
    // 1. Преобразование world coords → screen coords
    Point pt = RemapCoords(x, y, z);

    // 2. Проверка видимости
    if (outside viewport) return;

    // 3. Добавление в parent_sprites_to_draw
    ParentSpriteToDraw &ps = _vd.parent_sprites_to_draw.emplace_back();
    ps.image = image;     // SpriteID
    ps.pal = pal;         // PaletteID (для recolouring)
    ps.x = pt.x;         // Screen X
    ps.y = pt.y;         // Screen Y
    ps.sub = sub;         // SubSprite clipping rectangle
    // + bounding box для сортировки (xmin/ymin/zmin/xmax/ymax/zmax)
}
```

## ParentSpriteToDraw — ключевая структура

Файл: `src/viewport_sprite_sorter.h:16-38`

```cpp
struct ParentSpriteToDraw {
    // Bounding box в world coordinates (для Z-сортировки)
    int32_t xmin, ymin, zmin;
    int32_t x;              // screen X
    int32_t xmax, ymax, zmax;
    int32_t y;              // screen Y

    SpriteID image;         // Какой спрайт рисовать
    PaletteID pal;          // Палитра/ремап
    const SubSprite *sub;   // Клиппинг (nullable)

    int32_t left, top;      // Screen bounding box (для child sprites)
    int32_t first_child;    // Индекс первого child sprite (-1 = нет)
    uint32_t order;         // Для сортировки
};
```

**Важно для GPU**: эта структура содержит ВСЮ информацию для отрисовки одного спрайта.
`image` → SpriteID для lookup в атласе, `pal` → определяет BlitterMode и remap table,
`sub` → прямоугольник клиппинга (foundations, overlapping tiles).

## ViewportDrawer — аккумулятор спрайтов

Файл: `src/viewport.cpp:170-187`

```cpp
struct ViewportDrawer {
    DrawPixelInfo dpi;                                    // Текущая область рисования

    StringSpriteToDrawVector string_sprites_to_draw;      // Текст (названия городов, станций)
    TileSpriteToDrawVector tile_sprites_to_draw;          // Ground sprites
    ParentSpriteToDrawVector parent_sprites_to_draw;      // Основные спрайты (здания, транспорт)
    ParentSpriteToSortVector parent_sprites_to_sort;      // Указатели для сортировки
    ChildScreenSpriteToDrawVector child_screen_sprites_to_draw; // Детали поверх parent sprites

    int last_child;
    SpriteCombineMode combine_sprites;
    // + foundation tracking
};
```

Создаётся заново для КАЖДОГО вызова ViewportDoDraw. Все векторы clear() в конце.
**Это ключевая проблема** — вся работа по сбору спрайтов теряется между кадрами.

## Сортировка спрайтов (Z-order)

Файл: `src/viewport.cpp:1847`

```cpp
_vp_sprite_sorter(&_vd.parent_sprites_to_sort);
```

Спрайты сортируются по правилу isometric overlap:
- Сначала по Y (дальше от камеры → рисуется первым)
- Затем по X и Z для корректного перекрытия

Есть SSE4.1-оптимизированная версия. Занимает 0.1-0.5 ms — не bottleneck.

## Рисование спрайтов

Файл: `src/viewport.cpp:1715-1731`

```cpp
static void ViewportDrawParentSprites(const ParentSpriteToSortVector *psd,
                                       const ChildScreenSpriteToDrawVector *csstdv)
{
    for (const ParentSpriteToDraw *ps : *psd) {
        // Рисуем parent sprite
        DrawSpriteViewport(ps->image, ps->pal, ps->x, ps->y, ps->sub);

        // Рисуем все child sprites
        int child_idx = ps->first_child;
        while (child_idx >= 0) {
            const ChildScreenSpriteToDraw *cs = &(*csstdv)[child_idx];
            DrawSpriteViewport(cs->image, cs->pal, ...);
            child_idx = cs->next;
        }
    }
}
```

`DrawSpriteViewport()` → `GfxMainBlitterViewport()` → `GfxBlitter()` → `Blitter::Draw()`

**Это самая дорогая фаза** (1-17 ms): каждый спрайт композитится попиксельно в CPU buffer.

## Dirty Blocks: откуда берутся

### Транспорт
```
Vehicle::UpdatePosition() → MarkAllViewportsDirty()
    → для каждого viewport: MarkViewportDirty() → AddDirtyBlock()
```

### Анимация тайлов (вода, маяки)
```
AnimateTile() → MarkTileDirtyByTile()
    → MarkAllViewportsDirty() → AddDirtyBlock()
```

### Палитрная анимация
```
CheckPaletteAnim() → MakeDirty(0, 0, width, height)  // ВЕСЬ экран!
```

### Проблема coalescing

С `_gles_video_active = true` все dirty blocks сливаются в один прямоугольник.
Если 5 транспортов разбросаны по экрану, bounding rect = почти весь viewport.
Но без coalescing — 5+ отдельных RedrawScreenRect вызовов, каждый с overhead.

**Решение для GPU**: retained scene graph — обновлять только спрайты изменившихся объектов.
