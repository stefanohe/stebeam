// SteBeam - laser beam profiler
// Copyright (C) 2026 Stefano's AI Lab
// SPDX-License-Identifier: LGPL-3.0-or-later
//
// This library is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as
// published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this library.  If not, see <https://www.gnu.org/licenses/>.

// Shared color scale (single source of truth for the 2D pseudocolor view and the 3D surface --
// both render the same dark-background rainbow): three-segment 256-level ramp --
// dark gray at the minimum -> transitional dark blue -> full jet rainbow (blue->cyan->green->yellow->red).
// The view background is the same dark gray.
#pragma once
#include <QColor>
#include <QRgb>
#include <algorithm>

// View background (letterbox around the 2D image, 3D scene floor): dark gray rather than pure black
inline QColor viewBgColor() { return QColor(0x22, 0x22, 0x26); }

// 256-level LUT: minimum = dark gray, with a dedicated gray->blue transition segment (indices 0..38);
// dark blue is the jet starting point, the full jet rainbow sits above it. Both segments are dark
// blue at idx=38, so the ramp is continuous.
inline void buildJetLut(QRgb* lut)
{
	const int gr = 0x2c, gg = 0x2c, gb = 0x33;   // dark gray (minimum; slightly brighter than the background so the beam edge stays visible)
	const int br = 0x00, bg = 0x00, bb = 0x7f;   // dark blue (jet start)
	constexpr int grayEnd = 38;                  // end of the gray->blue transition segment
	for (int i = 0; i <= grayEnd; ++i) {
		double k = i / (double)grayEnd;
		lut[i] = qRgb(gr + (int)((br - gr) * k), gg + (int)((bg - gg) * k), gb + (int)((bb - gb) * k));
	}
	for (int i = grayEnd + 1; i < 256; ++i) {
		double t = (i - grayEnd) / (double)(255 - grayEnd);
		double r = std::clamp(std::min(4.0 * t - 1.5, -4.0 * t + 4.5), 0.0, 1.0);
		double g = std::clamp(std::min(4.0 * t - 0.5, -4.0 * t + 3.5), 0.0, 1.0);
		double b = std::clamp(std::min(4.0 * t + 0.5, -4.0 * t + 2.5), 0.0, 1.0);
		lut[i] = qRgb((int)(r * 255), (int)(g * 255), (int)(b * 255));
	}
}
