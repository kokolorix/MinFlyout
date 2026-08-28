/**
 * \file Painting.h
 * \ingroup ui
 * \brief Colors and the two rounded-rectangle primitives, shared by the windows.
 *
 * The flyout and the touch overlay draw the same things - a rounded panel, zone
 * tiles, a highlighted tile - and they have to agree on what a zone looks like,
 * because the user sees both within one second of each other. So the palette
 * and the two drawing helpers live here instead of in one of the two windows.
 *
 * Layout metrics stay where they are used: each window has its own, and the
 * overlay works at finger size rather than pointer size.
 */
#pragma once

#include "Common.h"

namespace mfly {

/**
 * \brief Colors of one theme.
 */
struct Palette {
    COLORREF background;    ///< Window background.
    COLORREF border;        ///< Window and miniature border.
    COLORREF text;          ///< Normal text.
    COLORREF textDim;       ///< Captions, disabled items.
    COLORREF hover;         ///< Hovered item row.
    COLORREF separator;     ///< Separator line.
    COLORREF miniBack;      ///< Background of a miniature (the "screen").
    COLORREF zone;          ///< A zone tile.
    COLORREF accent;        ///< Hovered zone.
};

/**
 * \brief Reads the system accent color.
 *
 * Uses \c DwmGetColorizationColor so the highlighted zone matches what Windows
 * paints elsewhere; falls back to the blue of the application icon.
 *
 * \return Accent color as \c COLORREF.
 */
COLORREF AccentColor();

/**
 * \brief Builds the palette for the current theme.
 * \param dark \c true for dark mode.
 * \return The palette.
 */
Palette MakePalette(bool dark);

/**
 * \brief Fills a rounded rectangle in one color.
 * \param dc     Target device context.
 * \param r      Rectangle.
 * \param color  Fill color.
 * \param radius Corner radius.
 */
void FillRounded(HDC dc, const RECT& r, COLORREF color, int radius);

/**
 * \brief Draws the outline of a rounded rectangle.
 * \param dc     Target device context.
 * \param r      Rectangle.
 * \param color  Line color.
 * \param radius Corner radius.
 * \param width  Line width in pixels.
 */
void FrameRounded(HDC dc, const RECT& r, COLORREF color, int radius, int width = 1);

/**
 * \brief Creates a UI font from the system metrics.
 *
 * \param dpi   Target DPI.
 * \param bold  \c true for the semibold variant.
 * \param delta Point-size offset applied before scaling; negative shrinks.
 * \return New font; the caller takes ownership. Never \c nullptr - falls back
 *         to \c DEFAULT_GUI_FONT, which must not be deleted, so the caller may
 *         only pass the result to \c DeleteObject, which ignores stock objects.
 */
HFONT CreateUiFont(UINT dpi, bool bold, int delta);

}  // namespace mfly
