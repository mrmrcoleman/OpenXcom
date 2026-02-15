/*
 * Copyright 2010-2016 OpenXcom Developers.
 *
 * This file is part of OpenXcom.
 *
 * OpenXcom is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * OpenXcom is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with OpenXcom.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "Cursor.h"
#include <cmath>
#include <SDL.h>
#include "../Engine/Action.h"

namespace OpenXcom
{

/**
 * Sets up a cursor with the specified size and position
 * and hides the system cursor.
 * @note The size and position don't really matter since
 * it's a 9x13 shape, they're just there for inheritance.
 * @param width Width in pixels.
 * @param height Height in pixels.
 * @param x X position in pixels.
 * @param y Y position in pixels.
 */
Cursor::Cursor(int width, int height, int x, int y) : Surface(width, height, x, y), _color(0)
{
}

/**
 *
 */
Cursor::~Cursor()
{
}

/**
 * Automatically updates the cursor position
 * when the mouse moves.
 * @param action Pointer to an action.
 */
void Cursor::handle(Action *action)
{
	if (action->getDetails()->type == SDL_MOUSEMOTION)
	{
		setX((int)floor((action->getDetails()->motion.x - action->getLeftBlackBand()) / action->getXScale()));
		setY((int)floor((action->getDetails()->motion.y - action->getTopBlackBand()) / action->getYScale()));
	}
}

/**
 * Changes the cursor's base color.
 * @param color Color value.
 */
void Cursor::setColor(Uint8 color)
{
	_color = color;
	_redraw = true;
}

/**
 * Returns the cursor's base color.
 * @return Color value.
 */
Uint8 Cursor::getColor() const
{
	return _color;
}

/**
 * Helper: draw a line on an 8-bit surface using raw setPixel.
 * This avoids the SDL2_gfx lineColor bridge which creates (and clears)
 * a temporary software renderer for every call, wiping previous draws.
 */
static void rawLine(Surface *s, int x1, int y1, int x2, int y2, Uint8 color)
{
	int dx = abs(x2 - x1);
	int dy = abs(y2 - y1);
	int sx = (x1 < x2) ? 1 : -1;
	int sy = (y1 < y2) ? 1 : -1;
	int err = dx - dy;

	while (true)
	{
		s->setPixel(x1, y1, color);
		if (x1 == x2 && y1 == y2)
			break;
		int e2 = 2 * err;
		if (e2 > -dy) { err -= dy; x1 += sx; }
		if (e2 <  dx) { err += dx; y1 += sy; }
	}
}

/**
 * Draws a pointer-shaped cursor graphic.
 */
void Cursor::draw()
{
	Surface::draw();
	Uint8 color = _color;
	int x1 = 0, y1 = 0, x2 = getWidth() - 1, y2 = getHeight() - 1;

	lock();
	for (int i = 0; i < 4; ++i)
	{
		rawLine(this, x1, y1, x1, y2, color);            /* vertical edge */
		rawLine(this, x1, y1, x2, getWidth() - 1, color); /* diagonal edge */
		x1++;
		y1 += 2;
		y2--;
		x2--;
		color++;
	}
	this->setPixel(4, 8, --color);
	unlock();
}

}
