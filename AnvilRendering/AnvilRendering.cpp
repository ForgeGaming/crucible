// AnvilRendering.cpp : Defines the exported functions for the DLL application.
//

#include "stdafx.h"

#include "../Crucible/IPC.hpp"

#include "AnvilRendering.h"
#include "GAPI_render/NewIndicator.h"

#include <json/reader.h>
#include <json/writer.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include <commctrl.h>	// HOTKEYF_ALT
#include <d3dcompiler.h>

using namespace std;

#define CONCAT2(x, y) x ## y
#define CONCAT(x, y) CONCAT2(x, y)
#define LOCK(x) lock_guard<decltype(x)> CONCAT(lockGuard, __LINE__){x};

struct FreeHandle
{
	void operator()(HANDLE h)
	{
		CloseHandle(h);
	}
};

using Handle = unique_ptr<void, FreeHandle>;

void (*hlog)(const char *fmt, ...) = nullptr;


namespace ForgeEvent
{
	using namespace json;

	IPCClient forge_client;

	static bool SendEvent(const Object &object)
	{
		ostringstream sstr;
		Writer::Write(object, sstr);
		return forge_client.Write(sstr.str());
	}

	static Object EventCreate(const char *name)
	{
		return Object().Set("event", String(name));
	}

	bool MouseEvent(int x, int y, int wParam)
	{
		return SendEvent(EventCreate("mouse_event")
			.Set("x", Number(x))
			.Set("y", Number(y))
			.Set("wParam", Number(wParam)));
	}

	bool ShowBrowser(const std::string &name, LONG width, LONG height)
	{
		return SendEvent(EventCreate("show_browser")
			.Set("framebuffer_server", String(name))
			.Set("width", Number(width))
			.Set("height", Number(height)));
	}

	bool HideBrowser()
	{
		return SendEvent(EventCreate("hide_browser"));
	}

	bool CreateBookmark()
	{
		return SendEvent(EventCreate("create_bookmark"));
	}
}


TAKSI_INDICATE_TYPE m_eSquareIndicator; // state for the old square indicator

										// new indicator stuff, we should really wrap it up in a separate object
IndicatorEvent m_eIndicatorEvent; // current indicator event we're displaying
IndicatorEvent m_eIndicatorContinuousEvent; // if we were showing a continuous notification like 'streaming', it gets saved here so we can return to it after any timeout notifications like screenshots
unsigned char m_uIndicatorAlpha;  // 0-255 alpha value for indicator fadeout and pulsating effects
IndicatorAnimation m_eIndicatorAnimation; // animation effect we're using
ULONGLONG m_ulIndicatorEventStart; // time we started current indicator event
ULONGLONG m_ulIndicatorEventStop; // time we should stop current indicator event
ULONGLONG m_ulIndicatorLastUpdate; // time we last updated current indicator event


static IndicatorEvent currentIndicator = INDICATE_NONE;

void ShowCurrentIndicator(const std::function<void(IndicatorEvent, BYTE /*alpha*/)> &func)
{
	func(currentIndicator, 255);
}

mutex hotkeys_mutex;
static WORD hotkeys[HOTKEY_QTY] = { 0 };

namespace CrucibleCommand {

using namespace json;

static void HandleIndicatorCommand(Object &obj)
{
	static const map<string, IndicatorEvent> indicators = {
		{"idle",       INDICATE_NONE},
		{"capturing",  INDICATE_CAPTURING},
		{"mic_idle",   INDICATE_MIC_IDLE},
		{"mic_active", INDICATE_MIC_ACTIVE},
		{"mic_muted",  INDICATE_MIC_MUTED},
		{"enabled",    INDICATE_ENABLED},
		{"enabled_hotkey", INDICATE_ENABLED_HOTKEY},
		{"bookmark",   INDICATE_BOOKMARK},
	};

	auto indicator = static_cast<String>(obj["indicator"]).Value();

	auto elem = indicators.find(indicator);
	if (elem == end(indicators))
		return hlog("Got invalid indicator '%s'", indicator.c_str());

	currentIndicator = elem->second;
}

static void HandleForgeInfo(Object &obj)
{
	String event = obj["anvil_event"];
	if (event.Value().empty())
		return hlog("Got empty anvil_event name via forge_info");

	ForgeEvent::forge_client.Open(event.Value());
}

static void HandleUpdateSettings(Object &obj)
{
	auto UpdateHotkey = [&](HOTKEY_TYPE hotkey, const char *setting_name)
	{
		Object key_data = obj[setting_name];
		if (!key_data.HasMember("keycode"))
			return;

		Boolean meta = key_data["meta"];
		if (meta)
			return hlog("meta modifier not supported for hotkey '%s' (%s)", HotKeyTypeName(hotkey), setting_name);

		Boolean shift = key_data["shift"];
		Boolean ctrl = key_data["ctrl"];
		Boolean alt = key_data["alt"];
		Number code = key_data["keycode"];

		hotkeys[hotkey] = (shift ? HOTKEYF_SHIFT : 0) |
			(ctrl ? HOTKEYF_CONTROL : 0) |
			(alt ? HOTKEYF_ALT : 0) |
			static_cast<int>(code.Value());

		if (!hotkeys[hotkey])
			return;

		hlog("hotkey '%s' (%s) updated", HotKeyTypeName(hotkey), setting_name);
	};

	{
		LOCK(hotkeys_mutex);
		//UpdateHotkey(HOTKEY_Screenshot, "...?");
		UpdateHotkey(HOTKEY_Bookmark, "bookmark_key");
		UpdateHotkey(HOTKEY_Overlay, "highlight_key");
	}
}

static void HandleCommands(uint8_t *data, size_t size)
{
	static const map<string, void(*)(Object&)> handlers = {
		{"indicator", HandleIndicatorCommand},
		{"forge_info", HandleForgeInfo},
		{"update_settings", HandleUpdateSettings},
	};

	if (!data)
		return;
	
	try {
		Object obj;

		hlog("Anvil got: '%s'", data);
		istringstream ss{{data, data + size - 1}};
		Reader::Read(obj, ss);

		auto cmd = static_cast<String>(obj["command"]).Value();

		if (!cmd.length())
			return hlog("Got invalid command with 0 length");

		auto handler = handlers.find(cmd);
		if (handler != cend(handlers))
			handler->second(obj);
		else
			hlog("Got unknown command '%s' (%d)", cmd.c_str(), cmd.length());

	} catch (Exception &e) {
		hlog("Unable to process command in HandleCommands: %s", e.what());
	}
}

}

ProcessCompat g_Proc;

pD3DCompile s_D3DCompile = nullptr;

IndicatorManager indicatorManager;

IPCServer crucibleConnection;

ULONG_PTR gdi_token = 0;

HINSTANCE g_hInst = nullptr;
bool g_bUseDirectInput = true;
bool g_bUseKeyboard = true;
bool g_bBrowserShowing = false;

WORD GetHotKey(HOTKEY_TYPE t)
{
	if (t >= 0 && t < HOTKEY_QTY)
	{
		LOCK(hotkeys_mutex);
		return hotkeys[t];
	}
	return 0;
}


C_EXPORT bool overlay_init(void (*hlog_)(const char *fmt, ...))
{
	hlog = hlog_;
	hlog("Started overlay");

	GetModuleHandleEx(
		GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
		GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCTSTR)overlay_init, &g_hInst);

	Gdiplus::GdiplusStartupInput gdiplusStartupInput;
	Gdiplus::Status status = Gdiplus::GdiplusStartup(&gdi_token, &gdiplusStartupInput, NULL);

	indicatorManager.LoadImages();

	crucibleConnection.Start("AnvilRenderer" + to_string(GetCurrentProcessId()), CrucibleCommand::HandleCommands);

	return true;
}

void StopInputHook();
void overlay_d3d11_free();
void overlay_d3d10_free();
void overlay_d3d9_free();
void overlay_gl_free();

C_EXPORT void overlay_reset()
{
	StopInputHook();

	overlay_d3d11_free();
	overlay_d3d10_free();
	overlay_d3d9_free();
	overlay_gl_free();
}

C_EXPORT void overlay_free()
{
	overlay_reset();

	if (gdi_token)
		Gdiplus::GdiplusShutdown(gdi_token);
	hlog("Stopped overlay");
}

C_EXPORT void overlay_compile_dxgi_shaders(pD3DCompile compile)
{
	s_D3DCompile = compile;
}
