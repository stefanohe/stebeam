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

// Frame data: 16-bit single-channel grayscale, the common input of all beam analysis.
#pragma once
#include <cstdint>
#include <QtGlobal>
#include <vector>

// Nominal full-sensor size (upper bound for sensor cropping; the simulated source uses the same
// size; for real cameras the authoritative values are read back at runtime from the
// WidthMax/HeightMax nodes)
inline constexpr int kSensorW = 1280, kSensorH = 1024;

struct Frame {
	int w = 0;
	int h = 0;
	std::vector<uint16_t> px;   // w*h, row-major
	double ts = 0.0;            // acquisition timestamp (seconds, monotonic)
	qint64 captureMs = 0;       // capture instant in epoch ms (stamped by the frame producer; used for replay timestamps and realtime-CSV filenames)
	// Sensor cropping: a frame may be a sub-window of the full sensor, so position readouts must be
	// mapped back to full-sensor coordinates
	int originX = 0, originY = 0;   // frame's top-left corner within the full sensor (0,0 when uncropped)
	int sensorW = 0, sensorH = 0;   // full-sensor size (0 = same as frame, i.e. uncropped)

	bool valid() const { return w > 0 && h > 0 && (int)px.size() == w * h; }
};
