# Система спрайтов и Blitter

## Формат спрайтов

### CommonPixel — исходный формат

```cpp
// src/spriteloader/spriteloader.hpp
struct CommonPixel {
    uint8_t r, g, b;  // RGB каналы
    uint8_t a;         // Alpha канал
    uint8_t m;         // Mapping/Remap канал (0 = использовать RGB, >0 = palette index)
};
```

**Два типа пикселей в одном спрайте:**
- `m == 0`: прямой RGBA цвет (32bpp спрайты, NewGRF)
- `m > 0`: индекс палитры (8bpp спрайты, базовый набор OpenTTD)

Большинство базовых спрайтов (ландшафт, здания, транспорт) — palette-only (m > 0, rgb = 0).

### SpriteData — формат 32bpp blitter

```cpp
// src/blitter/32bpp_optimized.hpp
struct SpriteData {
    uint32_t offset[2][ZOOM_LEVEL_COUNT];  // Смещения для pixel data и n-data
    uint8_t data[];                         // Сжатые данные
};
```

Хранит два канала:
1. **Pixel data** (`offset[0]`): массив `Colour` (RGBA)
2. **N-data** (`offset[1]`): массив `uint16_t` — упакованные (remap_index:8 | brightness:8)

Данные RLE-сжаты: каждая строка начинается с offset на следующую строку,
затем чередуются runs: (count, transparent_flag).

### Zoom levels

Каждый спрайт хранит данные для нескольких zoom levels:
```cpp
enum class ZoomLevel : uint8_t {
    Min   = 0,  // 4x (самый ближний)
    In2x  = 1,  // 2x
    Normal = 2,  // 1x
    Out2x = 3,  // 0.5x
    Out4x = 4,  // 0.25x
    Out8x = 5,  // 0.125x
    End   = 6,
};
```

При рисовке используется соответствующий zoom level — спрайт уже подготовлен.

## BlitterMode — режимы рисования

```cpp
enum class BlitterMode : uint8_t {
    Normal,            // Простой RGBA blend
    ColourRemap,       // M-channel → remap[m] → palette lookup (company colours, cargo)
    Transparent,       // Затемнение фона (прозрачные здания)
    TransparentRemap,  // Ремап прозрачности
    CrashRemap,        // Серый (разбитый транспорт)
    BlackRemap,        // Чёрный (оверлей аварии)
};
```

### Как определяется mode

```cpp
// src/gfx.cpp:1012-1029
void DrawSpriteViewport(SpriteID img, PaletteID pal, int x, int y, const SubSprite *sub)
{
    if (HasBit(img, PALETTE_MODIFIER_TRANSPARENT)) {
        // Прозрачность: mode = Transparent или TransparentRemap
        _colour_remap_ptr = GetNonSprite(pal, SpriteType::Recolour) + 1;
        GfxMainBlitterViewport(..., BlitterMode::Transparent, ...);
    } else if (pal != PAL_NONE) {
        // Есть палитра: mode = ColourRemap (company colours, cargo)
        _colour_remap_ptr = GetNonSprite(pal, SpriteType::Recolour) + 1;
        GfxMainBlitterViewport(..., BlitterMode::ColourRemap, ...);
    } else {
        // Обычный: mode = Normal
        GfxMainBlitterViewport(..., BlitterMode::Normal, ...);
    }
}
```

**Remap table** (`_colour_remap_ptr`): 256-байтная таблица, где `remap[old_index] = new_index`.
Используется для company colours (синий → красный), грузов, и других перекрасок.

## SubSprite — клиппинг

```cpp
struct SubSprite {
    int left, top, right, bottom;  // Прямоугольник клиппинга в координатах спрайта
};
```

Используется для:
- **Foundations** — фундаменты зданий, где спрайт рисуется частично
- **Overlapping tiles** — тайлы перекрывают друг друга, нужно обрезать
- Передаётся в `GfxBlitter()` который вычисляет `skip_left/skip_top` и `width/height`

**Для GPU**: нужно или scissor test, или UV clipping в шейдере.

## Sprite Cache

```cpp
// src/spritecache.cpp
static std::vector<SpriteCache> _spritecache;  // ~70000+ спрайтов
```

- Default размер кэша: 4MB
- LRU вытеснение при заполнении
- `GetSprite(SpriteID, SpriteType)` — загружает из GRF файлов если нужно
- Спрайты декодируются через `Blitter::Encode()` в формат текущего блиттера

### Для GPU atlas

При `Blitter_GLES::Encode()` спрайт загружается как в CPU формат (для fallback),
так и в GPU atlas через `GLESSpriteAtlas::Upload()`.

Текущая реализация atlas:
- **Colour atlas**: RGBA текстуры (GL_RGBA)
- **Remap atlas**: M-channel текстуры (GL_LUMINANCE)
- Каждый zoom level загружается отдельно
- Lookup: `MakeGLESSpriteKey(sprite_data_ptr, zoom)` → `GLESSpriteEntry`

```cpp
struct GLESSpriteEntry {
    struct { int atlas_idx; float u0, v0, u1, v1; } colour;  // UV в colour atlas
    struct { int atlas_idx; float u0, v0, u1, v1; } remap;   // UV в remap atlas
    bool has_remap;      // Есть ли M-channel
    bool palette_only;   // Только M-channel (нет RGB)
};
```

## GfxBlitter — CPU композитинг

Файл: `src/gfx.cpp:1073-1165`

Финальная функция которая преобразует `(Sprite, position, mode, sub)` в `BlitterParams`
и вызывает `Blitter::Draw()`.

Шаги:
1. Применяет sprite offsets (x_offs, y_offs)
2. Вычисляет SubSprite clipping → skip_left, skip_top, width, height
3. Клиппит к DrawPixelInfo (viewport bounds)
4. Вычисляет dst pointer = base + top * pitch + left
5. Вызывает `blitter->Draw(&bp, mode, zoom)`

**Для GPU**: вся эта логика заменяется одним GPU draw command:
```
QueueDraw(sprite_key, screen_x, screen_y, width, height, skip, mode, remap_idx)
```

## Текущие GPU шейдеры (уже реализованы)

| Шейдер | BlitterMode | Что делает |
|--------|-------------|------------|
| `prog_normal` | Normal | `gl_FragColor = texture2D(colour_tex, uv)` |
| `prog_palette` | Normal (palette-only) | M → palette_tex lookup |
| `prog_remap` | ColourRemap | M → remap_table → palette → цвет |
| `prog_transparent` | Transparent | Затемнение фона |
| `prog_bgra` | — | CPU buffer BGRA→RGBA swizzle |
| `prog_solid` | — | Сплошная заливка (debug) |

**Не хватает**: CrashRemap, BlackRemap, TransparentRemap (редко используются).
