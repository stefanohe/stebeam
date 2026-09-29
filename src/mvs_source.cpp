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

// Implementation notes: LoadLibrary runtime binding (~14 C functions); the grabbing loop runs in
// MvsWorker; pixel format prefers Mono12 (falls back to Mono8 if the node write fails), and
// Mono10/12/16 are shifted MSB-aligned into the 16-bit scale.
// Hard rule: never read the ExposureTime node after StartGrabbing -- on this machine, Get either
// while grabbing or after stopping permanently kills the link (the root cause of the 0x80000000
// reports); node readbacks happen only before grabbing.
#include "mvs_source.h"
#include "i18n.h"
#include <QDateTime>
#include <windows.h>

#pragma push_macro("__stdcall")
#define WIN_CALL __stdcall
#include "MvCameraControl.h"
#pragma pop_macro("__stdcall")

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

// ---- runtime-bound API function pointers (signatures match MvCameraControl.h) ----
namespace mv {
// Function-pointer types always carry the Fn prefix: a member named the same as its type shadows the typedef inside the struct and breaks the cast.
typedef int  (__stdcall *FnInitialize)();
typedef int  (__stdcall *FnFinalize)();
typedef int  (__stdcall *FnEnumDevices)(unsigned, MV_CC_DEVICE_INFO_LIST*);
typedef int  (__stdcall *FnCreateHandle)(void**, const MV_CC_DEVICE_INFO*);
typedef int  (__stdcall *FnDestroyHandle)(void*);
typedef int  (__stdcall *FnOpenDevice)(void*, unsigned, unsigned short);
typedef int  (__stdcall *FnCloseDevice)(void*);
typedef int  (__stdcall *FnStartGrabbing)(void*);
typedef int  (__stdcall *FnStopGrabbing)(void*);
typedef int  (__stdcall *FnGetImageBuffer)(void*, MV_FRAME_OUT*, unsigned);
typedef int  (__stdcall *FnFreeImageBuffer)(void*, MV_FRAME_OUT*);
typedef int  (__stdcall *FnSetEnumValue)(void*, const char*, unsigned);
typedef int  (__stdcall *FnSetIntValueEx)(void*, const char*, int64_t);
typedef int  (__stdcall *FnSetFloatValue)(void*, const char*, float);
typedef int  (__stdcall *FnGetFloatValue)(void*, const char*, MVCC_FLOATVALUE*);   // 3rd arg = struct pointer (per the SDK header; the old typedef passed float* and wrote 28 bytes over a 4-byte stack slot -- caught during a selfcam regression hunt)
typedef int  (__stdcall *FnSetBoolValue)(void*, const char*, bool);   // frame-rate switch AcquisitionFrameRateEnable
typedef int  (__stdcall *FnGetIntValueEx)(void*, const char*, MVCC_INTVALUE_EX*);   // integer node read (WidthMax/HeightMax caps + sensor node readback, before grabbing only); 3rd arg = struct pointer (per the SDK header, 96 bytes -- never pass a bare int64_t*)

struct Api {
	HMODULE lib = nullptr;
	FnInitialize Initialize = nullptr;
	FnFinalize Finalize = nullptr;
	FnEnumDevices EnumDevices = nullptr;
	FnCreateHandle CreateHandle = nullptr;
	FnDestroyHandle DestroyHandle = nullptr;
	FnOpenDevice OpenDevice = nullptr;
	FnCloseDevice CloseDevice = nullptr;
	FnStartGrabbing StartGrabbing = nullptr;
	FnStopGrabbing StopGrabbing = nullptr;
	FnGetImageBuffer GetImageBuffer = nullptr;
	FnFreeImageBuffer FreeImageBuffer = nullptr;
	FnSetEnumValue SetEnumValue = nullptr;
	FnSetIntValueEx SetIntValueEx = nullptr;
	FnSetFloatValue SetFloatValue = nullptr;
	FnGetFloatValue GetFloatValue = nullptr;
	FnSetBoolValue SetBoolValue = nullptr;
	FnGetIntValueEx GetIntValueEx = nullptr;

	bool load() {
		static const char* paths[] = {
			"C:\\Program Files (x86)\\Common Files\\MVS\\Runtime\\Win64_x64\\MvCameraControl.dll",
			"MvCameraControl.dll",   // next to the exe or on PATH
		};
		for (const char* p : paths) {
			lib = LoadLibraryA(p);
			if (lib) break;
		}
		return lib != nullptr;
	}
	bool bindAll() {
		if (!lib) return false;
		auto b = [&](const char* n) { return (void*)GetProcAddress(lib, n); };
		Initialize    = (FnInitialize)b("MV_CC_Initialize");
		Finalize      = (FnFinalize)b("MV_CC_Finalize");
		EnumDevices   = (FnEnumDevices)b("MV_CC_EnumDevices");
		CreateHandle  = (FnCreateHandle)b("MV_CC_CreateHandle");
		DestroyHandle = (FnDestroyHandle)b("MV_CC_DestroyHandle");
		OpenDevice    = (FnOpenDevice)b("MV_CC_OpenDevice");
		CloseDevice   = (FnCloseDevice)b("MV_CC_CloseDevice");
		StartGrabbing = (FnStartGrabbing)b("MV_CC_StartGrabbing");
		StopGrabbing  = (FnStopGrabbing)b("MV_CC_StopGrabbing");
		GetImageBuffer= (FnGetImageBuffer)b("MV_CC_GetImageBuffer");
		FreeImageBuffer=(FnFreeImageBuffer)b("MV_CC_FreeImageBuffer");
		SetEnumValue  = (FnSetEnumValue)b("MV_CC_SetEnumValue");
		SetIntValueEx = (FnSetIntValueEx)b("MV_CC_SetIntValueEx");
		SetFloatValue = (FnSetFloatValue)b("MV_CC_SetFloatValue");
		GetFloatValue = (FnGetFloatValue)b("MV_CC_GetFloatValue");
		SetBoolValue  = (FnSetBoolValue)b("MV_CC_SetBoolValue");
		GetIntValueEx = (FnGetIntValueEx)b("MV_CC_GetIntValueEx");   // optional binding (used to read caps before grabbing; falls back to nominal sensor size when absent)
		return Initialize && EnumDevices && CreateHandle && OpenDevice &&
		       StartGrabbing && GetImageBuffer && FreeImageBuffer && SetEnumValue;
	}
};
} // namespace mv

// MVS error code -> human-readable text (cross-referenced with MvErrorDefine.h; 0x80000000 = invalid
// handle = link lost -- showing a bare hex code in the status bar helps nobody)
static QString mvErrText(quint32 r)
{
	QString name;
	// Error wording follows the UI language switch; g_cn is read-only after construction, so reading it from the worker thread is safe
	switch (r) {
	case 0x80000000u: name = L("错误或无效的句柄（相机连接已断——被 MVS 客户端等程序抢占或 USB 断开；切回模拟源再切回 CMOS 即重连）",
	                           "Invalid handle (camera link lost — preempted by another app such as MVS, or USB disconnected; switch to Sim and back to CMOS to reconnect)"); break;
	case 0x80000001u: name = L("不支持的功能", "Unsupported function"); break;
	case 0x80000002u: name = L("缓存已满", "Buffer full"); break;
	case 0x80000003u: name = L("函数调用顺序错误", "Wrong call order"); break;
	case 0x80000004u: name = L("错误的参数", "Invalid parameter"); break;
	case 0x80000006u: name = L("资源申请失败", "Resource allocation failed"); break;
	case 0x80000007u: name = L("无数据", "No data"); break;
	case 0x80000008u: name = L("前置条件有误或运行环境已变化", "Precondition failed or runtime state changed"); break;
	case 0x8000001Au: name = L("设备无响应", "Device not responding"); break;
	case 0x800000FFu: name = L("未知错误", "Unknown error"); break;
	case 0x80000100u: name = L("GenICam 通用错误（节点类型/权限不符）", "GenICam generic error (node type/permission mismatch)"); break;
	case 0x80000103u: name = L("GenICam 读写超时", "GenICam read/write timeout"); break;
	default: break;
	}
	return name.isEmpty() ? QString("0x%1").arg(r, 8, 16, QChar('0'))
	                      : QString("%1，0x%2").arg(name).arg(r, 8, 16, QChar('0'));
}

// ---- grabbing thread ----
class MvsWorker : public QObject
{
	Q_OBJECT
public:
	std::atomic<bool> stopping{ false };  // only quit() sets it true and run() never clears it -- the old `running` flag was overwritten by running=true under a quit race, so the thread couldn't be stopped (freeze root cause)
	std::atomic<bool> guiBusy{ false };   // true = GUI still processing the previous frame; drop this frame (latest-value delivery, queue depth <= 1)
	std::atomic<double> pendExposure{ -1 };
	std::atomic<double> pendGain{ -1 };
	std::atomic<double> pendFps{ -1 };     // frame-rate mailbox: -1 = nothing pending
	// Sensor-crop mailbox: the four roi* values are written first and the pendRoi flag last
	// (seq_cst ordering guarantees the worker sees all four once it sees true)
	std::atomic<bool> pendRoi{ false };
	int roiW = 0, roiH = 0, roiXc = 0, roiYc = 0;   // center in full-sensor bottom-left origin (written by MvsSource::setRoi on the main thread)
	int sensorW = 0, sensorH = 0;    // full-sensor size (WidthMax/HeightMax readback before grabbing; nominal fallback on failure)
	int curOX = 0, curOY = 0;        // current acquisition window's top-left in full-sensor top-left coordinates (written into Frame.originX/originY per frame)
	double expReadback = -1;   // exposure node readback (written by the worker, read by the main thread via MvsSource::exposureUs; write frequency = send frequency)
	mv::Api api;

signals:
	void frameReady(const Frame& f);
	void statusText(const QString& s);
	void connectionChanged(bool connected);   // forwarded through MvsSource to the CameraSource interface

public slots:
	void run()
	{
		if (!api.load() || !api.bindAll()) {
			emit statusText(L("MvCameraControl.dll 加载失败（MVS 未安装或不在预期路径）",
			                  "Failed to load MvCameraControl.dll (MVS not installed or not at expected path)"));
			return;
		}
		api.Initialize();
		MV_CC_DEVICE_INFO_LIST list;
		std::memset(&list, 0, sizeof(list));
		int ret = api.EnumDevices(MV_USB_DEVICE, &list);
		if (ret != 0 || list.nDeviceNum == 0) {
			emit statusText(L("未发现 USB 相机 (ret=0x%1, n=%2)", "No USB camera found (ret=0x%1, n=%2)")
			                .arg(ret, 8, 16).arg(list.nDeviceNum));
			api.Finalize();
			return;
		}
		void* h = nullptr;
		if (api.CreateHandle(&h, list.pDeviceInfo[0]) != 0 ||
		    api.OpenDevice(h, MV_ACCESS_Exclusive, 0) != 0) {
			emit statusText(L("相机打开失败（可能被 MVS 客户端占用）", "Failed to open camera (possibly occupied by the MVS client)"));
			if (h) api.DestroyHandle(h);
			api.Finalize();
			return;
		}
		api.SetEnumValue(h, "TriggerMode", 0);                       // free-run
		// 12-bit acquisition depth: falls back to Mono8 if the node write fails; the worker converts Mono8 via MSB<<4 into the 16-bit scale
		bool mono12 = (api.SetEnumValue(h, "PixelFormat", PixelType_Gvsp_Mono12) == 0);
		if (!mono12) api.SetEnumValue(h, "PixelFormat", PixelType_Gvsp_Mono8);
		api.SetEnumValue(h, "ExposureMode", 0);                      // Timed -- the ExposureTime register is unwritable in other modes
		api.SetEnumValue(h, "ExposureAuto", 0);                      // disable auto exposure so the manual value holds (the camera may retain MVS-client settings)
		api.SetEnumValue(h, "GainAuto", 0);                          // disable auto gain, same reason
		// Initial sensor-node read + send: reading WidthMax/HeightMax caps and the current Width/Offset
		// nodes before grabbing is safe (the no-read rule only applies after StartGrabbing); curOX/curOY
		// are written into every Frame so position readouts stay correct even when the camera retains an
		// MVS-client ROI. Consuming pendRoi here = the safe pre-grab Set+Get readback path (same as exposure init).
		if (api.GetIntValueEx) {
			MVCC_INTVALUE_EX iv = {};
			if (api.GetIntValueEx(h, "WidthMax", &iv) == 0 && iv.nCurValue > 0) sensorW = (int)iv.nCurValue;
			if (api.GetIntValueEx(h, "HeightMax", &iv) == 0 && iv.nCurValue > 0) sensorH = (int)iv.nCurValue;
			if (api.GetIntValueEx(h, "OffsetX", &iv) == 0) curOX = (int)iv.nCurValue;
			if (api.GetIntValueEx(h, "OffsetY", &iv) == 0) curOY = (int)iv.nCurValue;
		}
		if (sensorW <= 0) sensorW = kSensorW;   // fall back to nominal sensor size on node-read failure
		if (sensorH <= 0) sensorH = kSensorH;
		if (pendRoi.load()) {
			pendRoi = false;
			int w = std::min(roiW & ~3, sensorW), hh = std::min(roiH & ~3, sensorH);   // 4-multiple alignment + cap clamp
			int ox = std::max(0, std::min(roiXc - w / 2, sensorW - w)) & ~3;
			int oyTop = std::max(0, std::min(sensorH - roiYc - hh / 2, sensorH - hh)) & ~3;   // bottom-left center -> OffsetY in the top-left system
			// Root fix for "500->1280 needs two Apply clicks": three-stage write -- zero the old Offsets
			// first, then write the new sizes (GenICam constrains Offset+Width <= WidthMax, so writing a
			// large Width against a stale Offset gets clamped), and finally land the target Offsets.
			int r0a = api.SetIntValueEx(h, "OffsetX", 0), r0b = api.SetIntValueEx(h, "OffsetY", 0);
			int r1 = api.SetIntValueEx(h, "Width", w), r2 = api.SetIntValueEx(h, "Height", hh);
			int r3 = api.SetIntValueEx(h, "OffsetX", ox), r4 = api.SetIntValueEx(h, "OffsetY", oyTop);
			MVCC_INTVALUE_EX rw = {}, rh = {}, rox = {}, roy = {};
			bool rbOk = api.GetIntValueEx && api.GetIntValueEx(h, "Width", &rw) == 0
			            && api.GetIntValueEx(h, "Height", &rh) == 0
			            && api.GetIntValueEx(h, "OffsetX", &rox) == 0
			            && api.GetIntValueEx(h, "OffsetY", &roy) == 0;
			if (rbOk) { curOX = (int)rox.nCurValue; curOY = (int)roy.nCurValue; }   // readback is truth (a value != requested means the camera's alignment rules adjusted the node)
			if (r0a || r0b || r1 || r2 || r3 || r4)
				emit statusText(L("靶面初始下发 %1 px × %2 px 失败：%3", "Failed to send initial sensor ROI %1 px x %2 px: %3")   // the geometric center is deliberately not printed here, to avoid confusion with the beam center
				                .arg(w).arg(hh)
				                .arg(mvErrText((quint32)(r0a ? r0a : r0b ? r0b : r1 ? r1 : r2 ? r2 : r3 ? r3 : r4))));
			else if (rbOk)
				emit statusText(L("靶面初始下发 %1 px × %2 px，节点回读 %3 px × %4 px%5",
				                  "Initial sensor ROI %1 px x %2 px, node readback %3 px x %4 px%5")
				                .arg(w).arg(hh).arg(rw.nCurValue).arg(rh.nCurValue)
				                .arg((rw.nCurValue != w || rh.nCurValue != hh || rox.nCurValue != ox || roy.nCurValue != oyTop)
				                     ? L("（≠请求值！）", " (!= requested)") : QString()));
			else
				emit statusText(L("靶面 %1 px × %2 px 已写，但节点回读失败", "Sensor ROI %1 px x %2 px written, but node readback failed").arg(w).arg(hh));
		}
		// Send and read back the initial exposure before grabbing: the old code consumed pendExposure only
		// after StartGrabbing, so the capture probe's --exp often missed it; and "in effect" relied solely on
		// the Set return code -- the real criterion is GetFloatValue reading back the node's actual value.
		if (pendExposure.load() >= 0) {
			double e0 = pendExposure.exchange(-1);
			if (api.SetFloatValue(h, "ExposureTime", (float)e0) == 0) {
				MVCC_FLOATVALUE rf = {};
				float rb = 0;
				if (api.GetFloatValue && api.GetFloatValue(h, "ExposureTime", &rf) == 0 && (rb = rf.fCurValue, true)) {
					expReadback = rb;
					emit statusText(L("曝光初始下发 %1 ms，节点回读 %2 ms%3", "Initial exposure %1 ms, node readback %2 ms%3")   // messages display ms (the internal mailbox stays in us)
					                .arg(e0 / 1000.0, 0, 'f', 3).arg(rb / 1000.0, 0, 'f', 3)
					                .arg(std::fabs((double)rb - e0) > e0 * 0.01
					                     ? L("（≠请求值！）", " (!= requested)") : QString()));
				} else emit statusText(L("曝光 %1 ms 已写，但节点回读失败", "Exposure %1 ms written, but node readback failed").arg(e0 / 1000.0, 0, 'f', 3));
			} else emit statusText(L("曝光初始下发 %1 ms 失败", "Failed to send initial exposure %1 ms").arg(e0 / 1000.0, 0, 'f', 3));
		}
		// Initial frame-rate send: Set+Get is safe before grabbing (same path as exposure init); two nodes, the Enable switch + the target value
		if (pendFps.load() >= 0) {
			double f0 = pendFps.exchange(-1);
			bool okEn = api.SetBoolValue && api.SetBoolValue(h, "AcquisitionFrameRateEnable", true) == 0;
			int rf = okEn ? api.SetFloatValue(h, "AcquisitionFrameRate", (float)f0) : -1;
			if (rf == 0) {
				MVCC_FLOATVALUE rf2 = {};
				float rb = 0;
				bool rbOk = api.GetFloatValue && api.GetFloatValue(h, "AcquisitionFrameRate", &rf2) == 0 && (rb = rf2.fCurValue, true);
				emit statusText(rbOk
					? L("帧率初始下发 %1 Hz，节点回读 %2 Hz%3", "Initial frame rate %1 Hz, node readback %2 Hz%3")
					      .arg(f0, 0, 'f', 1).arg(rb, 0, 'f', 1)
					      .arg(std::fabs((double)rb - f0) > f0 * 0.05 ? L("（≠请求值！）", " (!= requested)") : QString())
					: L("帧率 %1 Hz 已写，但节点回读失败", "Frame rate %1 Hz written, but node readback failed").arg(f0, 0, 'f', 1));
			} else {
				emit statusText(L("帧率初始下发 %1 Hz 失败（%2）", "Failed to send initial frame rate %1 Hz (%2)")
				                .arg(f0, 0, 'f', 1).arg(mvErrText((quint32)rf)));
			}
		}
		if (api.StartGrabbing(h) != 0) {
			emit statusText(L("StartGrabbing 失败", "StartGrabbing failed"));
			api.CloseDevice(h); api.DestroyHandle(h); api.Finalize();
			return;
		}
		emit statusText(L("相机已连接并开始取流（%1 台 USB 设备，%2bit）", "Camera connected, streaming started (%1 USB device(s), %2-bit)")
		                .arg(list.nDeviceNum).arg(mono12 ? 12 : 8));
		emit connectionChanged(true);   // the criterion is StartGrabbing succeeding

		MV_FRAME_OUT fo;
		int nEmit = 0, nDrop = 0;
		int nBufFail = 0;            // consecutive GetImageBuffer timeouts (500ms each): criterion for the plain-language stream-stall warning
		bool bufWarned = false;
		while (!stopping) {   // no running=true flag: if quit arrives first the loop never enters, so no overwrite race exists
			// Parameter-send failures must be reported (the old implementation dropped them silently, making the status bar's "applied" a lie)
			// ExposureTime must go through SetFloatValue: measured on this machine the node is an IFloat; SetIntValueEx returns 0x80000100 GC_GENERIC
			if (pendExposure.load() >= 0) {
				double e = pendExposure.exchange(-1);
				Sleep(120);   // spinbox-click / slider coalescing window: repeated edits within the window apply the latest value once, avoiding redundant writes
				double e2 = pendExposure.exchange(-1);
				if (e2 >= 0) e = e2;
				// While streaming: write only, never read -- settled by three isolation-probe groups (the real
				// root cause of "applying a parameter shows 0x80000000"): after StartGrabbing, any
				// GetFloatValue("ExposureTime") (a control-channel read, streaming or not) permanently kills the
				// link (frame fetches then always time out and later calls report 0x80000000 invalid handle);
				// SetFloatValue alone while streaming is safe and physically effective (frame mean scales with
				// exposure); the pre-grab initial path keeps Set+Get safe, so node-readback verification stays
				// only on that path. The mid-stream effectiveness criterion = Set return code (GenICam
				// validates the range at write time, accepted = in effect) + physical frame-mean verification.
				int r = api.SetFloatValue(h, "ExposureTime", (float)e);
				if (r != 0)
					emit statusText(L("曝光 %1 ms 下发失败：%2", "Failed to send exposure %1 ms: %2").arg(e / 1000.0, 0, 'f', 3).arg(mvErrText((quint32)r)));
				else {
					expReadback = e;   // accepted = in effect; the power formula's t_exp reads this value via exposureUs()
					emit statusText(L("曝光 %1 ms 已写并生效",
					                  "Exposure %1 ms written and in effect").arg(e / 1000.0, 0, 'f', 3));   // short message (the status label moved to the tool belt)
				}
			}
			if (pendGain.load() >= 0) {
				double g = pendGain.exchange(-1);
				int r = api.SetFloatValue(h, "Gain", (float)g);
				if (r != 0)
					emit statusText(L("增益 %1 dB 下发失败：%2", "Failed to send gain %1 dB: %2").arg(g, 0, 'f', 3).arg(mvErrText((quint32)r)));   // gain shown with 3 decimals
			}
			// Frame-rate send while streaming: write only, never read -- same iron rule as ExposureTime (a mid-stream node Get permanently kills the link)
			if (pendFps.load() >= 0) {
				double hz = pendFps.exchange(-1);
				bool okEn = api.SetBoolValue && api.SetBoolValue(h, "AcquisitionFrameRateEnable", true) == 0;
				int r = okEn ? api.SetFloatValue(h, "AcquisitionFrameRate", (float)hz) : -1;
				if (r != 0)
					emit statusText(L("帧率 %1 Hz 下发失败：%2", "Failed to send frame rate %1 Hz: %2").arg(hz, 0, 'f', 1).arg(mvErrText((quint32)r)));
				else
					emit statusText(L("帧率 %1 Hz 已写并生效", "Frame rate %1 Hz written and in effect").arg(hz, 0, 'f', 1));   // short message, no parenthetical
			}
				// Sensor-crop send: Width/Height/OffsetX/OffsetY are image-geometry nodes, which GenICam only
				// allows writing while stopped -- stop -> write -> restart (a brief stream gap is acceptable;
				// this is exactly the entry point for cutting data volume and raising the frame rate); no node
				// readback (the no-read rule: reading nodes after StartGrabbing kills the stream), so the
				// effectiveness criterion = Set return codes + the post-restart frame-size change (Frame.w/h reflects it naturally).
				if (pendRoi.load()) {
					pendRoi = false;
					int w = std::min(roiW & ~3, sensorW), hh = std::min(roiH & ~3, sensorH);   // 4-multiple alignment + cap clamp
					int ox = std::max(0, std::min(roiXc - w / 2, sensorW - w)) & ~3;
					int oyTop = std::max(0, std::min(sensorH - roiYc - hh / 2, sensorH - hh)) & ~3;   // bottom-left center -> OffsetY in the top-left system
					api.StopGrabbing(h);
					// Root fix for "500->1280 needs two Apply clicks": three-stage write -- zero the old
					// Offsets first, then write the new sizes (GenICam constrains Offset+Width <= WidthMax, so
					// writing a large Width against a stale Offset gets clamped and the first click only moved
					// the center), and finally land the target Offsets; no frames are captured during the
					// stopped intermediate state, so nothing is affected.
					int r0a = api.SetIntValueEx(h, "OffsetX", 0), r0b = api.SetIntValueEx(h, "OffsetY", 0);
					int r1 = api.SetIntValueEx(h, "Width", w), r2 = api.SetIntValueEx(h, "Height", hh);
					int r3 = api.SetIntValueEx(h, "OffsetX", ox), r4 = api.SetIntValueEx(h, "OffsetY", oyTop);
					int rs = api.StartGrabbing(h);
					if (r0a || r0b || r1 || r2 || r3 || r4 || rs)
						emit statusText(L("靶面 %1 px × %2 px 下发失败：%3", "Failed to send sensor ROI %1 px x %2 px: %3")
						                .arg(w).arg(hh)
						                .arg(mvErrText((quint32)(r0a ? r0a : r0b ? r0b : r1 ? r1 : r2 ? r2 : r3 ? r3 : r4 ? r4 : rs))));
					else {
						curOX = ox; curOY = oyTop;
						emit statusText(L("靶面已应用 %1 px × %2 px",
						                  "Sensor ROI %1 px x %2 px applied")
						                .arg(w).arg(hh));   // status message deliberately omits the center value
					}
				}

			if (api.GetImageBuffer(h, &fo, 500) == 0) {
				MV_FRAME_OUT_INFO_EX& i = fo.stFrameInfo;
				Frame f;
				f.w = i.nWidth; f.h = i.nHeight; f.ts = 0;
				f.captureMs = QDateTime::currentMSecsSinceEpoch();   // the SDK exposes no host timestamp; the local receive time is taken as the capture time
				f.originX = curOX; f.originY = curOY;   // sensor crop: the frame's top-left corner in full-sensor top-left coordinates, used to convert position readouts
				f.sensorW = sensorW; f.sensorH = sensorH;
				f.px.resize((size_t)f.w * f.h);
				// The whole pipeline is unified on the camera-native 12-bit ADU scale 0..4095 (the pseudocolor
				// range [-100,4096] requires values to line up directly with the 12-bit full scale; the old
				// 16-bit scale was a <<4 amplification).
				// Mono12 measured LSB-aligned 0..4095, taken directly; Mono8 <<4 (0..4080); Mono10/16 alignment unmeasured, fallback only.
				if (i.enPixelType == PixelType_Gvsp_Mono12) {
					std::memcpy(f.px.data(), fo.pBufAddr, f.px.size() * sizeof(uint16_t));
				} else if (i.enPixelType == PixelType_Gvsp_Mono16) {
					const uint16_t* src = (const uint16_t*)fo.pBufAddr;
					for (size_t k = 0; k < f.px.size(); ++k)
						f.px[k] = src[k] >> 4;
				} else if (i.enPixelType == PixelType_Gvsp_Mono10) {
					const uint16_t* src = (const uint16_t*)fo.pBufAddr;
					for (size_t k = 0; k < f.px.size(); ++k)
						f.px[k] = (uint16_t)((uint32_t)src[k] << 2);
				} else {
					const uint8_t* src = fo.pBufAddr;   // Mono8 and unknown 8-bit formats
					for (size_t k = 0; k < f.px.size(); ++k)
						f.px[k] = (uint16_t)((uint32_t)src[k] << 4);
				}
				api.FreeImageBuffer(h, &fo);
				// Backpressure drop: if the GUI is busy, drop this frame (no queuing, no blocking), preventing unbounded queued-event buildup
				if (!guiBusy.exchange(true))
					{ emit frameReady(f); ++nEmit; }
				else ++nDrop;
				nBufFail = 0;
			} else if (++nBufFail >= 20 && !bufWarned) {   // ~10 s with no frames = link lost (preempted / USB unplugged); warn once
				bufWarned = true;
				emit statusText(L("帧流中断（连续 10s 无帧）——相机可能被其他程序抢占或 USB 已断开；切回模拟源再切回 CMOS 即重连",
				                  "Frame stream interrupted (no frames for 10 s) — camera may be preempted by another program or USB disconnected; switch to Sim and back to CMOS to reconnect"));
				emit connectionChanged(false);
			}
		}
		printf("[mvs] worker exit: emit=%d drop=%d\n", nEmit, nDrop); fflush(stdout);
		api.StopGrabbing(h);
		api.CloseDevice(h);
		api.DestroyHandle(h);
		api.Finalize();
		emit statusText(L("取流线程已退出", "Grabbing thread exited"));
		emit connectionChanged(false);   // disconnect report (normal stops already gray out via the source identity; this covers abnormal paths)
	}

	void quit() { stopping = true; }
};

// ---- main-thread side ----
// Worker lifecycle = one grabbing session: each start() creates and wires a fresh worker; stop() waits for
// the thread to exit (finished is connected to deleteLater). The old implementation built the worker once
// at construction, so the first stop deleted it and a later start fired started->run on a dangling pointer
// (the root cause of "switching back to Sim then to the camera fails to reconnect"); guiBusy and friends
// reset naturally with the new worker.
MvsSource::MvsSource(QObject* parent) : CameraSource(parent)
{
}

MvsSource::~MvsSource()
{
	stop();
}

bool MvsSource::start()
{
	printf("[mvs] start: isRunning=%d\n", m_thread.isRunning() ? 1 : 0); fflush(stdout);
	if (m_thread.isRunning())
		return true;
	auto* w = new MvsWorker;
	m_worker = w;
	w->moveToThread(&m_thread);
	connect(&m_thread, &QThread::started, w, &MvsWorker::run);
	connect(&m_thread, &QThread::finished, w, &QObject::deleteLater);
	connect(w, &MvsWorker::frameReady, this, &CameraSource::frameReady);
	connect(w, &MvsWorker::statusText, this, &CameraSource::statusText);
	connect(w, &MvsWorker::connectionChanged, this, &CameraSource::connectionChanged);
	m_thread.start();
	return true;
}

void MvsSource::frameConsumed()
{
	if (m_worker) m_worker->guiBusy = false;   // atomic write, safe to call cross-thread directly; m_worker is already nulled after stop
}

double MvsSource::exposureUs() const
{
	// While streaming, read the worker's live value -- the old inline version returned m_expReadback, which
	// was only written back at stop(), so mid-session the power formula's t_exp used the previous session's
	// stale value (a latent bug found while chasing 0x80000000).
	return m_worker ? m_worker->expReadback : m_expReadback;
}

void MvsSource::stop()
{
	printf("[mvs] stop: isRunning=%d\n", m_thread.isRunning() ? 1 : 0); fflush(stdout);
	if (!m_thread.isRunning()) { m_worker = nullptr; return; }
	if (m_worker) {
		m_expReadback = m_worker->expReadback;   // write the readback cache back to the main thread at session end (exposureUs keeps reporting the last known in-effect value afterwards)
		m_worker->quit();
	}
	m_thread.quit();
	if (!m_thread.wait(3000))   // timeout = the thread is stuck in a camera API call; the worker is eventually destroyed on finished, but the session did not stop cleanly, so leave a trace
		printf("[mvs] stop: wait timeout, grabber thread still alive\n");
	fflush(stdout);
	m_worker = nullptr;   // the worker is deleteLater'd on finished; null it so later set*/frameConsumed calls can't write a dangling pointer
}

bool MvsSource::setExposureUs(double us)
{
	if (!m_worker) return false;   // honestly report failure when not grabbing (the old implementation always returned true, misleading the status bar)
	// pendExposure has two consumption points: before StartGrabbing (covering the pre-grab send / probe --exp race window) + inside the loop (mid-stream parameter changes)
	m_worker->pendExposure = us;
	return true;
}

bool MvsSource::setGainDb(double db)
{
	if (!m_worker) return false;
	m_worker->pendGain = db;
	return true;
}

bool MvsSource::setFpsHz(double hz)
{
	if (!m_worker || hz <= 0) return false;   // honestly report failure when not grabbing (same policy as setExposureUs)
	m_worker->pendFps = hz;
	return true;
}

// Sensor crop: center in full-sensor bottom-left-origin coordinates (y up, the whole app's unified convention); the
// conversion happens in the worker (sensorH lives worker-side). The four values are written first and the pendRoi flag
// last (seq_cst ordering). Two consumption points, same as exposure: the pre-grab initial path (Set+Get readback) +
// inside the loop (stop -> write -> restart).
bool MvsSource::setRoi(int w, int h, int xc, int ycBottom)
{
	if (!m_worker) return false;   // honestly report failure when not grabbing (same policy as setExposureUs)
	if (w < 4 || h < 4) return false;
	m_worker->roiW = w; m_worker->roiH = h; m_worker->roiXc = xc; m_worker->roiYc = ycBottom;
	m_worker->pendRoi = true;
	return true;
}

#include "mvs_source.moc"
