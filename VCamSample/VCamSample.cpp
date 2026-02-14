#include "framework.h"
#include "tools.h"
#include "VCamSample.h"

#include <cwctype>

#define MAX_LOADSTRING 100

// 3cad447d-f283-4af4-a3b2-6f5363309f52
static GUID CLSID_VCam = { 0x3cad447d,0xf283,0x4af4,{0xa3,0xb2,0x6f,0x53,0x63,0x30,0x9f,0x52} };
constexpr PCWSTR kGstConfigPath = L"SOFTWARE\\VCamSample\\GStreamer";
constexpr PCWSTR kPipelineValueName = L"Pipeline";
constexpr PCWSTR kWidthValueName = L"Width";
constexpr PCWSTR kHeightValueName = L"Height";
constexpr PCWSTR kFpsNumValueName = L"FpsNumerator";
constexpr PCWSTR kFpsDenValueName = L"FpsDenominator";

HINSTANCE _instance;
WCHAR _title[MAX_LOADSTRING];
WCHAR _windowClass[MAX_LOADSTRING];
wil::com_ptr_nothrow<IMFVirtualCamera> _vcam;
DWORD _vcamCookie;

ATOM MyRegisterClass(HINSTANCE hInstance);
HWND InitInstance(HINSTANCE, int);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK About(HWND, UINT, WPARAM, LPARAM);
HRESULT RegisterVirtualCamera();
HRESULT UnregisterVirtualCamera();
HRESULT StoreGstreamerPipelineConfig(PCWSTR pipelineText);

std::wstring Trim(std::wstring value)
{
	const auto isTrimChar = [](wchar_t ch) { return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n'; };
	while (!value.empty() && isTrimChar(value.front()))
	{
		value.erase(value.begin());
	}
	while (!value.empty() && isTrimChar(value.back()))
	{
		value.pop_back();
	}

	if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"')
	{
		value = value.substr(1, value.size() - 2);
	}
	return value;
}

bool TryParseUIntToken(const std::wstring& text, const std::wstring& token, DWORD* outValue)
{
	const auto pos = text.find(token);
	if (pos == std::wstring::npos)
	{
		return false;
	}

	auto cursor = pos + token.size();
	if (cursor >= text.size() || !iswdigit(text[cursor]))
	{
		return false;
	}

	DWORD value = 0;
	while (cursor < text.size() && iswdigit(text[cursor]))
	{
		value = value * 10 + (text[cursor] - L'0');
		cursor++;
	}

	*outValue = value;
	return true;
}

bool TryParseFramerateToken(const std::wstring& text, DWORD* numerator, DWORD* denominator)
{
	constexpr PCWSTR token = L"framerate=";
	const auto pos = text.find(token);
	if (pos == std::wstring::npos)
	{
		return false;
	}

	auto cursor = pos + wcslen(token);
	if (cursor >= text.size() || !iswdigit(text[cursor]))
	{
		return false;
	}

	DWORD num = 0;
	while (cursor < text.size() && iswdigit(text[cursor]))
	{
		num = num * 10 + (text[cursor] - L'0');
		cursor++;
	}

	DWORD den = 1;
	if (cursor < text.size() && text[cursor] == L'/')
	{
		cursor++;
		if (cursor >= text.size() || !iswdigit(text[cursor]))
		{
			return false;
		}

		den = 0;
		while (cursor < text.size() && iswdigit(text[cursor]))
		{
			den = den * 10 + (text[cursor] - L'0');
			cursor++;
		}
	}

	if (!num || !den)
	{
		return false;
	}

	*numerator = num;
	*denominator = den;
	return true;
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow)
{
	UNREFERENCED_PARAMETER(hPrevInstance);

	// set tracing & CRT leak tracking
	WinTraceRegister();
	WINTRACE(L"WinMain starting '%s'", GetCommandLineW());
	_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_DEBUG);

	wil::SetResultLoggingCallback([](wil::FailureInfo const& failure) noexcept
		{
			wchar_t str[2048];
			if (SUCCEEDED(wil::GetFailureLogString(str, _countof(str), failure)))
			{
				WinTrace(2, 0, str); // 2 => error
#ifndef _DEBUG
				TaskDialog(nullptr, nullptr, _title, L"A fatal error has occured. Press OK to terminate.", str, TDCBF_OK_BUTTON, TD_WARNING_ICON, nullptr);
#endif
			}
		});

	LoadStringW(hInstance, IDS_APP_TITLE, _title, MAX_LOADSTRING);
	LoadStringW(hInstance, IDC_VCAMSAMPLE, _windowClass, MAX_LOADSTRING);
	MyRegisterClass(hInstance);
	auto hwnd = InitInstance(hInstance, nCmdShow);
	if (hwnd)
	{
		winrt::init_apartment();
		if (SUCCEEDED(MFStartup(MF_VERSION)))
		{
			auto configHr = StoreGstreamerPipelineConfig(lpCmdLine);
			if (FAILED(configHr))
			{
				TASKDIALOGCONFIG cfg{};
				cfg.cbSize = sizeof(TASKDIALOGCONFIG);
				cfg.hInstance = hInstance;
				cfg.hwndParent = hwnd;
				cfg.pszWindowTitle = _title;
				cfg.dwCommonButtons = TDCBF_CLOSE_BUTTON;
				cfg.pszMainInstruction = L"Could not persist GStreamer pipeline configuration.";
				wchar_t text[1024];
				wchar_t errorText[256];
				FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, configHr, 0, errorText, _countof(errorText), nullptr);
				wsprintf(text, L"Error 0x%08X (%u): %s\nRun VCamSample elevated to update HKLM settings.", configHr, configHr, errorText);
				cfg.pszContent = text;
				cfg.pszMainIcon = TD_ERROR_ICON;
				TaskDialogIndirect(&cfg, nullptr, nullptr, nullptr);
			}
			else
			{
				TASKDIALOGCONFIG config{};
				config.cbSize = sizeof(TASKDIALOGCONFIG);
				config.hInstance = hInstance;
				config.hwndParent = hwnd;
				config.pszWindowTitle = _title;
				config.dwCommonButtons = TDCBF_CLOSE_BUTTON;
				auto hr = RegisterVirtualCamera();
				if (SUCCEEDED(hr))
				{
					config.pszMainInstruction = L"VCam was started, you can now run a program such as Windows Camera to visualize the output.\nPress Close to stop VCam and exit this program.";
					config.pszContent = L"This may stop VCam access for visualizing programs too.";
					config.pszMainIcon = TD_INFORMATION_ICON;
					TaskDialogIndirect(&config, nullptr, nullptr, nullptr);
					UnregisterVirtualCamera();
				}
				else
				{
					config.pszMainInstruction = L"VCam could not be started. Make sure you have registered the VCamSampleSource dll and configured a valid NV12 appsink pipeline.\nPress Close to exit this program.";
					wchar_t text[1024];
					wchar_t errorText[256];
					FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, hr, 0, errorText, _countof(errorText), nullptr);
					wsprintf(text, L"Error 0x%08X (%u): %s", hr, hr, errorText);
					config.pszContent = text;
					config.pszMainIcon = TD_ERROR_ICON;
					TaskDialogIndirect(&config, nullptr, nullptr, nullptr);
				}

				_vcam.reset();
			}

			MFShutdown();
		}
	}

	// cleanup & CRT leak checks
	_CrtDumpMemoryLeaks();
	WINTRACE(L"WinMain exiting '%s'", GetCommandLineW());
	WinTraceUnregister();
	return 0;
}
HRESULT RegisterVirtualCamera()
{
	auto clsid = GUID_ToStringW(CLSID_VCam);
	RETURN_IF_FAILED_MSG(MFCreateVirtualCamera(
		MFVirtualCameraType_SoftwareCameraSource,
		MFVirtualCameraLifetime_Session,
		MFVirtualCameraAccess_CurrentUser,
		_title,
		clsid.c_str(),
		nullptr,
		0,
		&_vcam),
		"Failed to create virtual camera");

	WINTRACE(L"RegisterVirtualCamera '%s' ok", clsid.c_str());
	RETURN_IF_FAILED_MSG(_vcam->Start(nullptr), "Cannot start VCam");
	WINTRACE(L"VCam was started");
	return S_OK;
}

HRESULT StoreGstreamerPipelineConfig(PCWSTR pipelineText)
{
	auto pipeline = Trim(pipelineText ? pipelineText : L"");
	if (pipeline.empty())
	{
		return S_OK;
	}

	HKEY key = nullptr;
	auto status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kGstConfigPath, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
	RETURN_HR_IF(HRESULT_FROM_WIN32(status), status != ERROR_SUCCESS);

	status = RegSetValueExW(key, kPipelineValueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(pipeline.c_str()), static_cast<DWORD>((pipeline.size() + 1) * sizeof(wchar_t)));
	if (status == ERROR_SUCCESS)
	{
		DWORD width = 0;
		DWORD height = 0;
		DWORD fpsNum = 0;
		DWORD fpsDen = 0;

		if (TryParseUIntToken(pipeline, L"width=", &width))
		{
			RegSetValueExW(key, kWidthValueName, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&width), sizeof(width));
		}

		if (TryParseUIntToken(pipeline, L"height=", &height))
		{
			RegSetValueExW(key, kHeightValueName, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&height), sizeof(height));
		}

		if (TryParseFramerateToken(pipeline, &fpsNum, &fpsDen))
		{
			RegSetValueExW(key, kFpsNumValueName, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&fpsNum), sizeof(fpsNum));
			RegSetValueExW(key, kFpsDenValueName, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&fpsDen), sizeof(fpsDen));
		}

		WINTRACE(L"Configured pipeline in HKLM from command line");
	}

	RegCloseKey(key);
	RETURN_HR_IF(HRESULT_FROM_WIN32(status), status != ERROR_SUCCESS);
	return S_OK;
}

HRESULT UnregisterVirtualCamera()
{
	if (!_vcam)
		return S_OK;

	// NOTE: we don't call Shutdown or this will cause 2 Shutdown calls to the media source and will prevent proper removing
	//auto hr = _vcam->Shutdown();
	//WINTRACE(L"Shutdown VCam hr:0x%08X", hr);

	auto hr = _vcam->Remove();
	WINTRACE(L"Remove VCam hr:0x%08X", hr);
	return S_OK;
}

ATOM MyRegisterClass(HINSTANCE instance)
{
	WNDCLASSEXW wcex{};
	wcex.cbSize = sizeof(WNDCLASSEX);
	wcex.style = CS_HREDRAW | CS_VREDRAW;
	wcex.lpfnWndProc = WndProc;
	wcex.hInstance = instance;
	wcex.hIcon = LoadIcon(instance, MAKEINTRESOURCE(IDI_VCAMSAMPLE));
	wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wcex.lpszMenuName = MAKEINTRESOURCEW(IDC_VCAMSAMPLE);
	wcex.lpszClassName = _windowClass;
	wcex.hIconSm = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));
	return RegisterClassExW(&wcex);
}

HWND InitInstance(HINSTANCE instance, int cmd)
{
	_instance = instance;
	auto hwnd = CreateWindowW(_windowClass, _title, WS_OVERLAPPEDWINDOW, 0, 0, 600, 400, nullptr, nullptr, instance, nullptr);
	if (!hwnd)
		return nullptr;

	CenterWindow(hwnd);
	ShowWindow(hwnd, cmd);
	UpdateWindow(hwnd);
	return hwnd;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	//#if _DEBUG
	//	if (message != WM_NCMOUSEMOVE && message != WM_MOUSEMOVE && message != WM_SETCURSOR && message != WM_NCHITTEST && message != WM_MOUSELEAVE &&
	//		message != WM_GETICON && message != WM_PAINT)
	//	{
	//		if (message == 147 || message == 148)
	//		{
	//			WINTRACE("msg:%u 0x%08X (%s)", message, message, WM_ToString(message).c_str());
	//		}
	//	}
	//#endif

	switch (message)
	{
	case WM_COMMAND:
	{
		auto wmId = LOWORD(wParam);
		switch (wmId)
		{
		case IDM_ABOUT:
			DialogBox(_instance, MAKEINTRESOURCE(IDD_ABOUTBOX), hwnd, About);
			break;

		case IDM_EXIT:
			DestroyWindow(hwnd);
			break;
		default:
			return DefWindowProc(hwnd, message, wParam, lParam);
		}
	}
	break;

	case WM_PAINT:
	{
		PAINTSTRUCT ps;
		auto hdc = BeginPaint(hwnd, &ps);
		// TODO: Add any drawing code that uses hdc here...
		EndPaint(hwnd, &ps);
	}
	break;

	case WM_DESTROY:
		PostQuitMessage(0);
		break;

	default:
		return DefWindowProc(hwnd, message, wParam, lParam);
	}
	return 0;
}

INT_PTR CALLBACK About(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	UNREFERENCED_PARAMETER(lParam);
	switch (message)
	{
	case WM_INITDIALOG:
		return (INT_PTR)TRUE;

	case WM_COMMAND:
		if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
		{
			EndDialog(hwnd, LOWORD(wParam));
			return (INT_PTR)TRUE;
		}
		break;
	}
	return (INT_PTR)FALSE;
}

