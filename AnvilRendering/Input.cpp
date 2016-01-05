#include "stdafx.h"

#include "AnvilRendering.h"

#include "TaksiInput/HotKeys.h"

#include "../Crucible/IPC.hpp"

#include <atomic>
#include <mutex>
#include <thread>

using namespace std;

static HWND win = nullptr;

static HHOOK mouse_hook = nullptr;

static void HookMouse()
{
	if (mouse_hook)
		return;

	auto thread_id = GetWindowThreadProcessId(win, nullptr);

	mouse_hook = SetWindowsHookEx(WH_MOUSE, [](int code, WPARAM wParam, LPARAM lParam) -> LRESULT
	{
		POINT last_pt;

		if (g_bBrowserShowing && code == HC_ACTION)
		{
			auto &mhs = *reinterpret_cast<MOUSEHOOKSTRUCT*>(lParam);
			last_pt = mhs.pt;

			SetLastError(0);
			if (MapWindowPoints(nullptr, win, &last_pt, 1) || GetLastError() == 0)
			{
				//hlog("mouse stuff: @%d,%d", last_pt.x, last_pt.y);
				ForgeEvent::MouseEvent(last_pt.x, last_pt.y, wParam);
			}

			switch (wParam)
			{
					case WM_LBUTTONDOWN:
					case WM_LBUTTONUP:
					case WM_LBUTTONDBLCLK:
					case WM_RBUTTONDOWN:
					case WM_RBUTTONUP:
					case WM_RBUTTONDBLCLK:
					case WM_MBUTTONDOWN:
					case WM_MBUTTONUP:
					case WM_MBUTTONDBLCLK:
					case WM_MOUSEWHEEL:
					case WM_XBUTTONDOWN:
					case WM_XBUTTONUP:
					case WM_XBUTTONDBLCLK:
						return 1;
			}
		}

		return CallNextHookEx(mouse_hook, code, wParam, lParam);
	}, NULL, thread_id);

	if (mouse_hook)
		hlog("hooked mouse events");
}

#define CONCAT2(x, y) x ## y
#define CONCAT(x, y) CONCAT2(x, y)
#define LOCK(x) lock_guard<decltype(x)> CONCAT(lockGuard, __LINE__){x}

static struct ForgeFramebufferServer {
	IPCServer server;

	std::string name;

	atomic<bool> died = true;
	atomic<bool> new_data = false;

	vector<uint8_t> read_data;
	vector<uint8_t> incoming_data;

	mutex share_mutex;
	vector<uint8_t> shared_data;

	void Start()
	{
		static int restarts = 0;
		died = false;

		name = "AnvilFramebufferServer" + to_string(GetCurrentProcessId()) + "-" + to_string(restarts++);

		auto expected = g_Proc.m_Stats.m_SizeWnd.cx * g_Proc.m_Stats.m_SizeWnd.cy * 4;

		server.Start(name, [&](uint8_t *data, size_t size)
		{
			if (!data) {
				died = true;
				hlog("AnvilFramebufferServer: died");
				return;
			}

			if (size != g_Proc.m_Stats.m_SizeWnd.cx * g_Proc.m_Stats.m_SizeWnd.cy * 4) {
				hlog("AnvilFramebufferServer: got invalid size: %d, expected %d", size, g_Proc.m_Stats.m_SizeWnd.cx * g_Proc.m_Stats.m_SizeWnd.cy * 4);
				return;
			}

			hlog("AnvilFramebufferServer: got size %d", size);

			incoming_data.assign(data, data + size);

			LOCK(share_mutex);
			swap(incoming_data, shared_data);
			new_data = true;
		}, expected > 1024 ? expected : -1);
	}

	void Stop()
	{
		server.server.reset();
		died = true;
		new_data = false;

		LOCK(share_mutex);
		read_data.clear();
		incoming_data.clear();
	}
} forgeFramebufferServer;

vector<uint8_t> *ReadNewFramebuffer()
{
	if (!forgeFramebufferServer.new_data)
		return nullptr;

	{
		LOCK(forgeFramebufferServer.share_mutex);
		swap(forgeFramebufferServer.shared_data, forgeFramebufferServer.read_data);
		forgeFramebufferServer.new_data = false;
	}

	return &forgeFramebufferServer.read_data;
}

static HCURSOR previous_cursor = nullptr;

void ShowCursor() // hook SetCursor instead?
{
	SetCursor(LoadCursorW(NULL, IDC_ARROW));
}

void ToggleOverlay()
{
	if (g_bBrowserShowing && forgeFramebufferServer.died)
		forgeFramebufferServer.Start();

	if (!g_bBrowserShowing) {
		forgeFramebufferServer.Start();
		ForgeEvent::ShowBrowser(forgeFramebufferServer.name, g_Proc.m_Stats.m_SizeWnd.cx, g_Proc.m_Stats.m_SizeWnd.cy);
		hlog("Requesting browser");
		previous_cursor = SetCursor(LoadCursorW(NULL, IDC_ARROW));
	} else {
		ForgeEvent::HideBrowser();
		hlog("Hiding browser");
		forgeFramebufferServer.Stop();
		SetCursor(previous_cursor);
	}

	g_bBrowserShowing = !g_bBrowserShowing;
}

static void ProcessHotKeys()
{
	for (auto event : g_HotKeys.m_events)
	{
		switch (event.key)
		{
		case HOTKEY_Overlay:
			if (event.event == HKEVENT_PRESS)
				ToggleOverlay();
			break;
		case HOTKEY_Bookmark:
			if (event.event == HKEVENT_PRESS)
				ForgeEvent::CreateBookmark();
			break;
		case HOTKEY_Screenshot:
			break;
		}
	}

	g_HotKeys.m_events.clear();
}

void HandleInputHook(HWND window)
{
	if (!win)
	{
		g_Proc.m_Stats.m_hWndCap = window;
		win = window;
	}

	if (!g_HotKeys.HotkeysAttached())
		g_HotKeys.AttachHotKeysToApp();

	if (g_UserDI.m_bSetup)
		g_UserDI.ProcessDirectInput();

	HookMouse();

	ProcessHotKeys();
}

void StopInputHook()
{
	if (!win)
		return;

	win = nullptr;

	g_HotKeys.DetachHotKeys();
	UnhookWindowsHookEx(mouse_hook);
}