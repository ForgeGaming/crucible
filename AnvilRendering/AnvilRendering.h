#pragma once

#include <functional>

#define C_EXPORT extern "C" __declspec(dllexport)

extern void (*hlog)(const char *fmt, ...);

#define LOG_MSG(x, ...) hlog(x, __VA_ARGS__)
#define LOG_WARN LOG_MSG
#define LOG_CR

#ifdef _DEBUG
#define DEBUG_MSG LOG_MSG
#else
#define DEBUG_MSG(...)
#endif

#define DEBUG_TRACE(...)
#define DEBUG_ERR(...)

struct ProcessCompat {
	struct ProcStatsCompat {
		SIZE m_SizeWnd;					// the window/backbuffer size. (pixels)
		HWND m_hWndCap;
	} m_Stats;
};

extern ProcessCompat g_Proc;

class IndicatorManager;
extern IndicatorManager indicatorManager;

enum IndicatorEvent;
void ShowCurrentIndicator(const std::function<void(IndicatorEvent, BYTE /*alpha*/)> &func);

void HandleInputHook(HWND window);


enum HOTKEY_TYPE
{
	HOTKEY_Screenshot,
	HOTKEY_Bookmark,
	HOTKEY_Overlay,
	HOTKEY_QTY,
};

enum HOTKEY_EVENT
{
	HKEVENT_PRESS,
	HKEVENT_RELEASE,
	HKEVENT_QTY
};

WORD GetHotKey(HOTKEY_TYPE t);

void ToggleOverlay();

extern HINSTANCE g_hInst;
extern bool g_bUseDirectInput;
extern bool g_bUseKeyboard;
extern bool g_bBrowserShowing;

namespace ForgeEvent
{
	bool ShowBrowser(const std::string &name, LONG width, LONG height);
	bool HideBrowser();
}
