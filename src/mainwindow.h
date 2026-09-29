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

// Main window: top tool ribbon + left five-view stack (2D / profile / size chart / beam track / 3D,
// mutually exclusive frame feeding to save compute) + right brand banner with Home/Setup/Acquisition
// analysis tabs + QSettings persistence.
#pragma once
#include "beam_analyzer.h"
#include "camera_source.h"
#include "csv_recorder.h"
#include "frame.h"
#include "shutter_controller.h"
#include "track_view.h"   // TrackPoint definition (m_trackHist carries timestamps)
#include <QElapsedTimer>
#include <QMainWindow>
#include <QPointF>
#include <QVector>
#include <array>
#include <deque>

class LineChartView;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QRadioButton;
class QPushButton;
class QSlider;
class QSpinBox;
class QStackedWidget;
class QTabWidget;
class QToolBar;
class SpotView;
class TrackView;
class View3D;

class MainWindow : public QMainWindow
{
	Q_OBJECT
public:
	explicit MainWindow(QWidget* parent = nullptr);
	void selfCamTest();   // --selfcam: programmatically switch camera sources under offscreen and report frame counts (freeze-regression probe)
	void selfViewsTest(); // --selfviews: switch through the five views under offscreen, grab each and count non-background pixels (effectiveness check for exclusive feeding + 3D custom paint)
	void selfCsvTest();   // --selfcsv: simulated source + save every frame, count realtime_csv files on disk and read back sizes after 3 s (effectiveness check for realtime CSV saving)

private slots:
	void onFrame(const Frame& f);
	void onSourceToggled();
	void applyCameraParams();
	void applyRoi();                        // apply the sensor crop (send setRoi; the camera side stops the stream to reconfigure)
	void effAcqSize(int& w, int& h) const;  // configured size -> actual sent size after maintain-size enlargement (shared by applyRoi/rotKeepCheck)
	void rotKeepCheck();                    // resend only when the needed acquisition size differs from what was sent (angle-change / auto-leveling debounce)
	void shutterOpen();
	void shutterClose();
	void startRoiDraw();
	void savePng();
	void saveCsv();
	void saveRaw();
	void showAbout();                       // open-source compliance: the About box shows the LGPL-3.0 Appropriate Legal Notices (copyright / warranty disclaimer / source URL / third-party pointer)
	QString csvHeader2d(const AnalysisResult& r) const;   // 2D intensity CSV metadata header block (6 English lines, same template for static/realtime)
	void onViewPage(int page);            // view switch: stack page change + button group sync + re-feed the newest frame to the new page
	void resetSettingsIni();

private:
	void buildUi();
	QToolBar* buildRibbon();               // top tool ribbon (Capture/File/Config groups)
	QWidget* buildViews();                // left QStackedWidget with five views + bottom switch-button row
	QWidget* buildPanel();                // right brand banner + Home/Setup/Acquisition tabs
	double bgValue() const;   // -1 when "custom unit background power" is unchecked (analyzer takes the ROI median), else the manually entered value
	void attachSource(CameraSource* src);
	void setAcquiring(bool on);             // acquisition start/stop: call the active source's start/stop + sync the button state
	void showDisplay();                     // replay system: feed the current page with the live or replayed frame/result per m_rbSel (2D/3D/profile + Home readouts follow; stats/size/track always live)
	void onReplaySel(int idx);              // replay position changed (-1 = live): sync slider/position box + refresh display + buffer-cap change linkage
	QString viewTitle() const;              // current view title (default name for PNG/CSV/RAW saves = view title)
	void pushStat(std::deque<double>& q, double v);
	static QStringList statLines(const std::deque<double>& q, double scale = 1.0, int dec = 1, const QString& unit = QString());   // beam-width stat decimals follow the unit (px 1, um/mm 3); returns {mean·sigma line, min·max line}; when unit is non-empty it trails every value (names drop the parenthesized unit, one unit convention across the app)
	double sizeScale() const;             // px -> current size unit factor
	QString sizeUnit() const;
	double powWPerAdu() const;            // watts/ADU (0 = ADU mode, no conversion); readouts are indicative only until camera constants are calibrated
	QString fmtPower(double adu) const;
	double effPower(const AnalysisResult& r) const;   // effective power = r.power - background power when subtraction is checked and background was captured (clamped >= 0)
	QString powUnitLabel() const;         // power-row unit column (auto range-selection result)
	QString powUnitForAdu(double adu) const;   // unit tier an ADU value should display with under fmtPower (same range-selection source as fmtPower)
	QString convPowTxt(double adu) const;      // ADU -> global power unit conversion text (tier chosen by magnitude, no truncation -- nonzero values get enough decimals for 3 significant digits)
	QString fmtSize(double pxVal) const;  // size readout (unit conversion + significant digits)
	void rebuildSizeChart();              // fully rebuild the curve after time-window/unit changes
	void updateReadouts(const AnalysisResult& r);   // refresh the Home three-column readouts (incl. the primary diameter chosen by the definition combo)
	void updateProfileChart(const AnalysisResult& r);   // refresh the profile chart (called only while the profile page is visible)
	void updateBkgState();              // background-subtraction state hint + button availability (shutter present -> auto-calibrate ready; absent -> gated by occlusion detection)
	void refreshBkgLabels();            // background-power / unit-background readouts = ADU main value + parenthesized global unit (recomputed per unit tier; custom-checked unit background shows the manual override)
	void syncRoiLimits();               // sensor center spin ranges follow the size (note refresh split out into updateRoiNote)
	void updateRoiNote(int w, int h, int ox, int oy, int sw, int sh);   // sensor range note = the actual covered interval of the applied window (data source = frame geometry, changes only after Apply)
	void applySettingsDefaults();         // restore defaults = rebuild every control's default value (no disk write)
	QString iniPath() const;              // settings.ini path (exe directory)
	void saveSettingsIni(const QString& path);          // ribbon Config group: settings persistence (dialog picks location + custom file name)
	void loadSettingsIni(const QString& path, bool quiet);   // quiet=true = silent auto-load at startup (missing keys keep defaults); false = button load, dialog first, then report the result

	SpotView* m_view = nullptr;
	CameraSource* m_sim = nullptr;
	CameraSource* m_mvs = nullptr;      // lazily created (on first switch to the camera source)
	CameraSource* m_active = nullptr;
	ShutterController* m_shutter = nullptr;

	// Background power subtraction: with a shutter = close it, sample the stray-light residual (sum ADU), restore the shutter, power readouts may subtract it;
	// without a shutter (not everyone owns one) = the user manually blocks the spot before sampling, occlusion detection gates the button
	QPushButton* m_bkgBtn = nullptr;
	QLabel* m_bkgState = nullptr;      // state hint after the button (ready / block manually / spot not blocked / spot blocked)
	QCheckBox* m_subBkg = nullptr;
	QCheckBox* m_rawBgSub = nullptr;      // subtract background from raw intensity data (2D/3D/CSV/RAW saves see the effective floor already deducted)
	QLabel* m_bkgLabel = nullptr;
	QLabel* m_bkgUnitLabel = nullptr;   // unit-background row (below background power; recorded alongside it = mean effective floor during sampling; custom-checked shows the manual override)
	double m_bkgAdu = -1;          // captured background power ADU (<0 = not captured)
	double m_bkgUnitAdu = -1;      // unit-background ADU of the captured background period (<0 = not captured)
	bool m_bkgLblSampled = false;  // background-power label text is in sampled state (only refreshBkgLabels recomputes per unit tier; sampling/failed/not-captured states are not overwritten)
	bool m_bkgLblFromIni = false;  // the sampled value came from the previous session (text carries a "last capture" note)
	bool m_bkgBusy = false;
	bool m_shutterConnected = false;   // shutter present (startup probe + per-command reply, jointly judged)
	bool m_beamBlocked = false;        // manual occlusion detection: peak < 50% full scale (2048 ADU) counts as blocked
	// No software state tracking on the shutter side -- protocol replies are command echoes and the power-on physical state cannot be
	// reliably queried; storing an "original state" via m_shutterOpen=false once caused the shutter not to reopen after the first
	// background capture (root fix: reopen unconditionally).
	int m_bkgCount = 0, m_bkgTicks = 0;
	double m_bkgSum = 0;
	double m_bkgUnitSum = 0;       // sum of the effective floor during sampling (mean = captured unit background)

	QRadioButton* m_radioSim = nullptr;
	QRadioButton* m_radioCam = nullptr;
	QDoubleSpinBox* m_exp = nullptr;
	QDoubleSpinBox* m_gain = nullptr;
	QDoubleSpinBox* m_fps = nullptr;        // frame rate setting Hz (Apply sends AcquisitionFrameRateEnable + AcquisitionFrameRate)
	QLabel* m_fpsLive = nullptr;            // measured live frame rate (frame-interval EMA, shown in the acquisition-params group)
	// Sensor crop (cut data volume at acquisition to raise the frame rate): window size + center (bottom-left origin, y up)
	QSpinBox* m_roiW = nullptr;             // X acquisition size (cap = sensor width)
	QSpinBox* m_roiH = nullptr;             // Y acquisition size (cap = sensor height)
	QSpinBox* m_roiXc = nullptr;            // X window center (range follows the size)
	QSpinBox* m_roiYc = nullptr;            // Y window center (bottom-left origin)
	QPushButton* m_roiBtn = nullptr;        // apply sensor (camera side stop -> write -> restart, independent of the exposure Apply)
	QPushButton* m_roiTrack = nullptr;      // track centroid (fills the window center from the snapshot; auto-applies the sensor afterwards)
	QPushButton* m_roiP500 = nullptr;       // sensor quick preset 500x500 (doubles as the selftest probe's programmatic click port)
	QPushButton* m_roiP200 = nullptr;       // sensor quick preset 200x200
	int m_appliedRoiW = -1, m_appliedRoiH = -1;   // acquisition size actually sent to the source (maintain-size = enlarged value; -1 = never sent)
	qint64 m_rotKeepLastApplyMs = -10000;          // throttle timestamp for angle-driven resends (prevents repeated stream stops during convergence)
	int m_noteGeom[6] = { -1, -1, -1, -1, -1, -1 };   // sensor-range note geometry cache w,h,ox,oy,sensorW,sensorH (note refreshes only on frame-geometry change)
	QLabel* m_roiNoteX = nullptr;           // range note, three lines (X dim / Y dim / origin, aligned with the stats column; shows the actual sensor of the rotated view)
	QLabel* m_roiNoteY = nullptr;
	QLabel* m_roiNoteOrg = nullptr;
	double m_fpsEma = -1;
	qint64 m_lastFrameMs = -1;
	int m_fpsLiveTick = 0;
	QDoubleSpinBox* m_bg = nullptr;
	QCheckBox* m_bgCustom = nullptr;      // custom unit background power (checked = use the m_bg manual value, unchecked = per-frame median automatically; migrated and renamed from the old setup-page "auto" checkbox)
	QDoubleSpinBox* m_thr = nullptr;
	QLabel* m_camConn = nullptr;            // green connection badge after CMOS in the ribbon (driven by connectionChanged; the "camera status" message group was removed, so this badge is the single status outlet)
	QLabel* m_shutterStatus = nullptr;
	// Home three-column readouts (right-aligned label + value + unit)
	QComboBox* m_diamDef = nullptr;       // primary diameter definition 0=D4sigma(ISO) 1=84% energy 2=FWHM
	QCheckBox* m_showCross = nullptr;     // centroid crosshair toggle
	QLabel* m_rdPower = nullptr;
	QLabel* m_rdPkInt = nullptr;          // peak intensity (peak-pixel ADU converted on the same scale)
	QVector<QLabel*> m_powUnitLbl;        // unit column for power/peak-intensity rows (syncs with fmtPower's auto tier)
	QLabel* m_rdDox = nullptr, * m_rdDoy = nullptr, * m_rdEff = nullptr;
	QLabel* m_rdEllip = nullptr, * m_rdOrient = nullptr;
	QLabel* m_rdCx = nullptr, * m_rdCy = nullptr, * m_rdPkX = nullptr, * m_rdPkY = nullptr;
	QLabel* m_rdSat = nullptr;   // (the Home unit-background row was removed; redefined in the acquisition page's background-subtraction group)
	QVector<QLabel*> m_dUnitLbl;          // unit column for diameter rows (synced on size-unit change)
	QLabel* m_statPower = nullptr;     // the three stat groups each = prefix label + mean·sigma line + min·max line (grid col1 vertically aligned)
	QLabel* m_statPower2 = nullptr;
	QLabel* m_statWidthX = nullptr;
	QLabel* m_statWidthX2 = nullptr;
	QLabel* m_statWidthY = nullptr;
	QLabel* m_statWidthY2 = nullptr;
	QLabel* m_statPowLbl = nullptr;
	QLabel* m_statWxLbl = nullptr;
	QLabel* m_statWyLbl = nullptr;
	QLabel* m_roiLabel = nullptr;
	QCheckBox* m_pcAuto = nullptr;      // pseudocolor auto range (on by default = 0.5%~99.5% percentiles)
	QSlider* m_loSl = nullptr;          // pseudocolor lower-bound slider (12-bit ADU scale)
	QSlider* m_hiSl = nullptr;          // pseudocolor upper-bound slider
	QLabel* m_loLbl = nullptr;
	QLabel* m_hiLbl = nullptr;
	LineChartView* m_profileChart = nullptr;   // beam profile (custom-painted chart_view.h replaced QtCharts for the license change)
	LineChartView* m_sizeChart = nullptr;      // realtime size chart (84% energy widths X/Y per frame, same custom painter)
	TrackView* m_track = nullptr;         // beam tracking (centroid trajectory scatter, custom-painted in track_view.h; QtCharts value axes cannot invert, hence the custom view)
	std::deque<TrackPoint> m_trackHist;   // recent centroid trajectory window (cap 2000 points, raw px; scaled by the size unit at TrackView paint;
	                                      // carries capture time in ms -- CSV export writes X/Y by timestamp)
	QCheckBox* m_show84 = nullptr;        // 84% enclosed-energy contour overlay toggle (in Home Main Controls)
	QComboBox* m_shape84 = nullptr;       // contour shape 0=ellipse 1=rectangle
	QCheckBox* m_rotSpot = nullptr;       // realtime spot rotation: per-frame leveling (ellipse major axis rotated to +Y, sensor cropped to the inscribed rectangle)
	QPushButton* m_rotOnce = nullptr;     // one-shot rotation button: computes the angle once at press time then holds it (no per-frame recompute),
	                                      // so sensor geometry and CSV matrix sizes stay stable
	bool m_rotOnceActive = false;         // one-shot mode active (toggling realtime exits it; pressing again re-measures once on top of the current angle)
	double m_rotOnceAngle = 0;            // fixed angle in one-shot mode (deg, image coordinates, positive = clockwise)
	int m_col0WDbg = 0;                   // acquisition-page name-column fixed width (for PANEL_DBG horizontal-scroll debugging)
	QCheckBox* m_rotCustom = nullptr;     // custom rotation angle toggle (off = automatic 50%-gated detection)
	QDoubleSpinBox* m_rotDeg = nullptr;   // custom rotation angle deg (-180~180, 0.1 deg precision, image coordinates, positive = clockwise)
	double m_rotAngle = 0;                // auto-leveling angle cache (re-detected every 8 frames for performance, reused in between)
	bool m_rotAngleValid = false;
	int m_rotAngleFrame = -100;
	// Maintain sensor size after rotation: when checked, rotation no longer crops to the inscribed rectangle but enlarges the acquisition
	// sensor to the bounding size of the rotated output (bounding version: output strictly = configured size, center-aligned, out-of-source
	// corner regions read 0); if the enlargement exceeds the sensor cap it clamps to the cap (= partial maintenance, black corners = honest
	// presentation within the cap).
	QCheckBox* m_rotKeepSize = nullptr;
	void exitOnceMode();                  // leave one-shot rotation mode (flag + button text/color/checkbox state all reset together)
	QComboBox* m_sizeUnit = nullptr;      // size unit 0=px 1=um 2=mm
	QComboBox* m_powUnit = nullptr;       // power unit 0=ADU 1=uW 2=mW 3=W
	QComboBox* m_winCombo = nullptr;      // acquisition time window 1/5/10/30/60 minutes
	QDoubleSpinBox* m_pixSize = nullptr;  // pixel size um/px (datasheet value, default 1 = unset)
	QDoubleSpinBox* m_waveNm = nullptr;   // laser wavelength nm
	QDoubleSpinBox* m_qePct = nullptr;    // quantum efficiency % (placeholder default, uncalibrated)
	QDoubleSpinBox* m_ePerAdu = nullptr;  // conversion gain e-/ADU@0dB (placeholder default, uncalibrated)
	QDoubleSpinBox* m_calCoef = nullptr;  // single-point calibration coefficient (fine-tuned against a power meter)
	QCheckBox* m_calCustom = nullptr;     // custom QE/conversion gain: off = built-in defaults (QE official response curve gray_line + log extrapolation, gain 1.0 placeholder), on = editable
	QDoubleSpinBox* m_bkgSec = nullptr;   // background power sampling duration (seconds, default 2, user-settable)
	QComboBox* m_3dQuality = nullptr;     // 3D resolution 0=standard 96x76 1=high 192x152 2=ultra 320x256
	QComboBox* m_3dSmooth = nullptr;      // 3D shading 0=fast (flat quad fill) 1=smooth (per-pixel interpolation)
	// Custom view axis bounds (off = auto, on = fixed range; size chart Y / profile Y / track X / track Y)
	QCheckBox* m_szYCustom = nullptr;
	QDoubleSpinBox* m_szYLo = nullptr;
	QDoubleSpinBox* m_szYHi = nullptr;
	QCheckBox* m_pfYCustom = nullptr;
	QDoubleSpinBox* m_pfYLo = nullptr;
	QDoubleSpinBox* m_pfYHi = nullptr;
	QCheckBox* m_trXCustom = nullptr;
	QDoubleSpinBox* m_trXLo = nullptr;
	QDoubleSpinBox* m_trXHi = nullptr;
	QCheckBox* m_trYCustom = nullptr;
	QDoubleSpinBox* m_trYLo = nullptr;
	QDoubleSpinBox* m_trYHi = nullptr;
	QElapsedTimer m_clock;                // chart time base
	std::deque<std::array<double, 3>> m_sizeHist;   // t seconds, d84X px, d84Y px (raw values, converted at display time)
	int m_winSec = 60;
	double m_l84cx = 0, m_l84cy = 0, m_l84wx = 0, m_l84wy = 0;   // most recent 84% contour parameters (for instant repaint on toggle)
	double m_l84rot = 0;                // most recent contour rotation angle (= the effective frame's orientation; always 0 while spot rotation is on)
	double m_l84rotAngle = 0;           // contour 50%-gated orientation cache (recomputed every 8 frames for performance)
	bool m_l84rotValid = false;
	int m_l84rotFrame = -100;

	// View stack and exclusive frame feeding (2D/3D switching saves compute)
	QStackedWidget* m_stack = nullptr;
	QTabWidget* m_tabs = nullptr;
	QPushButton* m_viewBtn[5] = {};
	QPushButton* m_langBtn = nullptr;     // top-right language switch (writes ini ui/lang, takes effect after restart)
	View3D* m_3d = nullptr;
	int m_effLo = -100, m_effHi = 4096;   // currently effective pseudocolor range (synced into 3D height normalization)

	Frame m_last;                         // display frame (with raw-intensity background subtraction checked = the floor already deducted; views/CSV/RAW saves all use it)
	Frame m_lastRaw;                      // effective raw frame (floor not deducted, for places that re-analyze such as one-shot rotation; prevents double subtraction)
	AnalysisResult m_lastR;               // latest analysis result (re-fed to a view on page switch)
	std::deque<double> m_statPowerQ, m_statWxQ, m_statWyQ;
	int m_framesSeen = 0;   // selfcam probe frame counter

	// Acquisition start/stop button (after the CMOS connection badge in the ribbon)
	QPushButton* m_acqBtn = nullptr;

	// 2D intensity replay system (up to 100 frames replayable, configurable/disableable; slider + position box, timestamp at bottom right;
	// 3D surface / beam profile / Home readouts follow the replayed frame, while stats/spot size/centroid tracking always use the live pipeline)
	struct ReplayItem { Frame fr; AnalysisResult r; };   // stores the rotated frame + its analysis = the display convention
	std::deque<ReplayItem> m_rb;
	int m_rbSel = -1;                     // -1 = live; 0..size-1 = replay index from oldest
	QPushButton* m_rbBtn = nullptr;       // replay/live toggle (slider disabled in live mode; switching to replay auto-stops acquisition, back to live auto-resumes)
	QSpinBox* m_rbMax = nullptr;          // replay frame cap (0 = feature off, no recording; default 100)
	QSlider* m_rbSl = nullptr;            // replay slider (rightmost = newest frame; draggable only in replay mode)
	QSpinBox* m_rbSpin = nullptr;         // jump to frame N (0 = newest, 1 = oldest...; replay mode only)
	QLabel* m_rbLbl = nullptr;            // frame number text (replay frame N of M)
	QLabel* m_tsLbl = nullptr;            // displayed frame's capture timestamp (right end of the control row = view's bottom-right corner)
	QWidget* m_replayRow = nullptr;       // replay control row (visible only on the 2D intensity page)
	QWidget* m_trackClearRow = nullptr;   // centroid-track clear row (visible only on the tracking page)
	QPushButton* m_trackClearBtn = nullptr;   // clear trajectory (clears m_trackHist; new points keep recording)

	// Realtime CSV saving (file-bar button; timestamp-named files in the default folder, every Nth frame, can stop live;
	// off by default, button semantics start/stop, storage folder customizable via ini setup/csvDir)
	QPushButton* m_csvBtn = nullptr;
	QSpinBox* m_csvEvery = nullptr;       // save every Nth frame
	QLabel* m_csvLbl = nullptr;           // saved file count (always shows the true count)
	QPushButton* m_csvDirBtn = nullptr;   // ... button picks a custom storage folder (tooltip shows the current path)
	CsvRecorder m_csv;                    // async disk-write worker (default exe\realtime_csv\ or user-chosen; the value member reaps the thread with the main window's destructor)
};
