# Рендеринг текста и UI

## Текст

### Шрифты = спрайты

OpenTTD хранит глифы шрифтов как обычные спрайты в GRF файлах.

```cpp
// src/fontcache/spritefontcache.cpp
// Глиф = SpriteID, загружается через стандартный GetSprite()
SpriteID GetUnicodeGlyph(FontSize fs, char32_t key);  // char → SpriteID

// Размеры шрифтов
enum FontSize { FS_NORMAL, FS_SMALL, FS_LARGE, FS_MONO };
// Базовые SpriteID: SPR_ASCII_SPACE, SPR_ASCII_SPACE_SMALL, SPR_ASCII_SPACE_BIG
```

### Call chain для отрисовки текста

```
DrawString(x, y, str, colour, ...)      // src/gfx.cpp
  └── DrawLayoutLine(layout, y, ...)
      └── for each glyph in layout:
          sprite = fc->GetGlyph(glyph_id)   // Sprite* из кэша шрифтов
          GfxMainBlitter(sprite, x, y, BlitterMode::ColourRemap, ...)
          // Каждый символ = отдельный вызов blitter!
```

**Для GPU**: каждый глиф уже спрайт → автоматически попадёт в GPU atlas.
Нужно только перенаправить GfxMainBlitter на GPU command queue вместо CPU blitter.
Тени текста рисуются отдельным проходом (сначала тень, потом текст).

### Recolour текста

Текст использует `BlitterMode::ColourRemap` с специальной таблицей:
```cpp
// src/gfx.cpp:78
static uint8_t _string_colourremap[3];  // 3 записи: [0]=прозрачный, [1]=тень, [2]=основной
```

Шрифтовые спрайты содержат только palette indices 0-2, которые ремапятся в:
- 0 → прозрачный
- 1 → цвет тени
- 2 → основной цвет текста

## UI примитивы

### GfxFillRect — заполнение прямоугольника

Файл: `src/gfx.cpp:118-164`

```cpp
void GfxFillRect(int left, int top, int right, int bottom,
                  const std::variant<PixelColour, PaletteID> &colour, FillRectMode mode)
{
    switch (mode) {
        case FILLRECT_OPAQUE:   // Сплошная заливка
            blitter->DrawRect(dst, width, height, colour);
            break;

        case FILLRECT_RECOLOUR: // Палитрный ремап (тонирование)
            blitter->DrawColourMappingRect(dst, width, height, palette_id);
            break;

        case FILLRECT_CHECKER:  // Шахматная заливка (полупрозрачность)
            // Рисует каждый второй пиксель
            for each row: for i = (bo ^= 1); i < right; i += 2:
                blitter->SetPixel(dst, i, 0, colour);
            break;
    }
}
```

**Для GPU**:
- FILLRECT_OPAQUE → `prog_solid` (уже есть)
- FILLRECT_RECOLOUR → новый шейдер или модификация existing
- FILLRECT_CHECKER → шейдер с `discard` для нечётных пикселей

### DrawFrameRect — 3D рамка виджета

```cpp
// src/widget.cpp:291-324
void DrawFrameRect(int left, int top, int right, int bottom, Colours colour, FrameFlags flags)
{
    // 4-5 вызовов GfxFillRect для каждого края рамки
    GfxFillRect(left, top, left, bottom - 1, dark);     // Левая грань
    GfxFillRect(left + 1, top, right - 1, top, dark);   // Верхняя грань
    GfxFillRect(right, top, right, bottom - 1, light);  // Правая грань
    GfxFillRect(left + 1, bottom, right, bottom, light); // Нижняя грань
    // + заливка внутренности если не BorderOnly
    GfxFillRect(left + 1, top + 1, right - 1, bottom - 1, fill);
}
```

Каждый UI виджет состоит из нескольких прямоугольников + текст + иконки (спрайты).

### GfxDrawLine — рисование линий

```cpp
// src/gfx.cpp:396
void GfxDrawLine(int left, int top, int right, int bottom, PixelColour colour, int width, int dash)
```

Использует Bresenham алгоритм, рисует попиксельно через `blitter->SetPixel()`.
Используется для: графики прибыли, соединительных линий, overlay маршрутов.

**Для GPU**: конвертировать в thin quad (прямоугольник) или GL_LINES.

## Полный список GPU-изируемых операций

| Операция | Текущий путь | GPU путь | Сложность |
|----------|-------------|----------|-----------|
| DrawSpriteViewport | Blitter::Draw → CPU pixels | GPU atlas quad | ✅ Уже частично сделано |
| DrawSprite (UI) | Blitter::Draw → CPU pixels | GPU atlas quad | Средняя |
| DrawString (текст) | Per-glyph Blitter::Draw | Per-glyph GPU atlas quad | Средняя |
| GfxFillRect OPAQUE | Blitter::DrawRect | prog_solid quad | ✅ Шейдер есть |
| GfxFillRect RECOLOUR | DrawColourMappingRect | Новый шейдер | Средняя |
| GfxFillRect CHECKER | Per-pixel SetPixel | Шейдер с discard | Простая |
| GfxDrawLine | Bresenham SetPixel | GL_LINES или thin quad | Простая |
| DrawFrameRect | 5× GfxFillRect | 5× solid quad | Автоматически |
| ScrollBuffer | Blitter::ScrollBuffer | FBO blit со смещением | Средняя |

## Что НЕ нужно переносить на GPU

1. **Window layout** — вычисление позиций виджетов (чистая логика, нет рендеринга)
2. **Text layout** — Layouter, line breaking (чистая логика)
3. **Game logic** — вся симуляция остаётся на CPU
4. **Sprite decoding** — GRF parsing, RLE декодирование (разовая операция при загрузке)
