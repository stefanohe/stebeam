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

// SteBeam entry point: --selftest (synthetic-frame self-test) / --capture (real-hardware capture
// probe) both run headless and re-attach to the caller's console; otherwise start the GUI.
#include "mainwindow.h"
#include <QApplication>
#include <QIcon>
#include <windows.h>
#include <cstdio>

int runSelftest();
int runCapture(int argc, char** argv);

int main(int argc, char* argv[])
{
	bool selftest = false, capture = false, selfcam = false, selfviews = false, selfcsv = false;
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--selftest") == 0) selftest = true;
		if (strcmp(argv[i], "--capture")  == 0) capture  = true;
		if (strcmp(argv[i], "--selfcam")  == 0) selfcam  = true;
		if (strcmp(argv[i], "--selfviews") == 0) selfviews = true;
		if (strcmp(argv[i], "--selfcsv") == 0) selfcsv = true;   // realtime CSV recorder probe
	}

	if (selftest || capture) {
		// WIN32 subsystem has no console: re-attach to the parent terminal and route stdout there.
		// But when stdout is already redirected (file or pipe) never touch it, or CONOUT$ would
		// clobber the redirection -- the test must be "not a character device", not "is a disk file".
		HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
		bool redirected = hOut && GetFileType(hOut) != FILE_TYPE_CHAR;
		if (!redirected) {
			if (AttachConsole(ATTACH_PARENT_PROCESS)) {
				freopen("CONOUT$", "w", stdout);
				freopen("CONOUT$", "w", stderr);
			}
		}
		return selftest ? runSelftest() : runCapture(argc, argv);
	}

	QApplication app(argc, argv);
	app.setWindowIcon(QIcon(":/logo/logo.png"));   // window/taskbar icon; app.ico (exe icon) is derived from the same artwork
	MainWindow w;
	w.show();
	if (selfcam) w.selfCamTest();
	if (selfviews) w.selfViewsTest();
	if (selfcsv) w.selfCsvTest();
	return app.exec();
}
