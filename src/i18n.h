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

// UI language infrastructure:
// g_cn is set once during MainWindow construction (from settings.ini ui/lang, default cn) and is
// read-only afterwards. L(zh, en) is the single source of truth for every UI string -- shared by
// main-window widgets and custom-painted views (spot_view/view3d/track_view), which resolve the
// wording at paint time, so the window never mixes languages under one switch.
// Term abbreviations (ROI / QE / D4sigma ...) stay identical in both languages.
// Device status messages (frame source / shutter replies) are routed through L() as well;
// g_cn being fixed and read-only after construction makes resolving them from worker threads safe.
#pragma once
#include <QString>

inline bool g_cn = true;

inline QString L(const char* zh, const char* en)
{
	return g_cn ? QString::fromUtf8(zh) : QString::fromUtf8(en);
}
