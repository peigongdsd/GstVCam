#include "framework.h"
#include "tools.h"

#include <cwctype>

// 3cad447d-f283-4af4-a3b2-6f5363309f52
static GUID CLSID_VCam = { 0x3cad447d,0xf283,0x4af4,{0xa3,0xb2,0x6f,0x53,0x63,0x30,0x9f,0x52} };
constexpr PCWSTR kVirtualCameraName = L"VCamSample";
constexpr PCWSTR kGstConfigPath = L"SOFTWARE\\VCamSample\\GStreamer";
constexpr PCWSTR kPipelineValueName = L"Pipeline";
constexpr PCWSTR kWidthValueName = L"Width";
constexpr PCWSTR kHeightValueName = L"Height";
constexpr PCWSTR kFpsNumValueName = L"FpsNumerator";
constexpr PCWSTR kFpsDenValueName = L"FpsDenominator";

wil::com_ptr_nothrow<IMFVirtualCamera> _vcam;
HANDLE g_stopEvent = nullptr;

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

std::wstring BuildPipelineFromArgs(int argc, wchar_t* argv[])
{
	std::wstring pipeline;
	for (int i = 1; i < argc; i++)
	{
		if (!pipeline.empty())
		{
			pipeline += L' ';
		}
		pipeline += argv[i];
	}
	return Trim(pipeline);
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

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType)
{
	switch (ctrlType)
	{
	case CTRL_C_EVENT:
	case CTRL_BREAK_EVENT:
	case CTRL_CLOSE_EVENT:
	case CTRL_LOGOFF_EVENT:
	case CTRL_SHUTDOWN_EVENT:
		if (g_stopEvent)
		{
			SetEvent(g_stopEvent);
		}
		return TRUE;
	default:
		return FALSE;
	}
}

int wmain(int argc, wchar_t* argv[])
{
	WinTraceRegister();
	WINTRACE(L"wmain starting '%s'", GetCommandLineW());
	_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_DEBUG);

	wil::SetResultLoggingCallback([](wil::FailureInfo const& failure) noexcept
		{
			wchar_t str[2048];
			if (SUCCEEDED(wil::GetFailureLogString(str, _countof(str), failure)))
			{
				WinTrace(2, 0, str); // 2 => error
			}
		});

	int exitCode = 0;
	winrt::init_apartment();

	const auto startupHr = MFStartup(MF_VERSION);
	if (FAILED(startupHr))
	{
		WINTRACE(L"MFStartup failed hr:0x%08X", startupHr);
		exitCode = 1;
		goto Exit;
	}

	{
		auto pipeline = BuildPipelineFromArgs(argc, argv);
		const auto configHr = StoreGstreamerPipelineConfig(pipeline.empty() ? nullptr : pipeline.c_str());
		if (FAILED(configHr))
		{
			WINTRACE(L"StoreGstreamerPipelineConfig failed hr:0x%08X (run elevated to write HKLM)", configHr);
			exitCode = 2;
			goto ShutdownMf;
		}
	}

	{
		const auto registerHr = RegisterVirtualCamera();
		if (FAILED(registerHr))
		{
			WINTRACE(L"RegisterVirtualCamera failed hr:0x%08X", registerHr);
			exitCode = 3;
			goto ShutdownMf;
		}
	}

	g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!g_stopEvent)
	{
		WINTRACE(L"CreateEvent failed err:%u", GetLastError());
		exitCode = 4;
		goto RemoveCamera;
	}

	SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
	WINTRACE(L"VCam started. Press Ctrl+C to stop.");
	WaitForSingleObject(g_stopEvent, INFINITE);
	SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
	CloseHandle(g_stopEvent);
	g_stopEvent = nullptr;

RemoveCamera:
	UnregisterVirtualCamera();

ShutdownMf:
	MFShutdown();

Exit:
	_CrtDumpMemoryLeaks();
	WINTRACE(L"wmain exiting '%s' code:%d", GetCommandLineW(), exitCode);
	WinTraceUnregister();
	return exitCode;
}

HRESULT RegisterVirtualCamera()
{
	auto clsid = GUID_ToStringW(CLSID_VCam);
	RETURN_IF_FAILED_MSG(MFCreateVirtualCamera(
		MFVirtualCameraType_SoftwareCameraSource,
		MFVirtualCameraLifetime_Session,
		MFVirtualCameraAccess_CurrentUser,
		kVirtualCameraName,
		clsid.c_str(),
		nullptr,
		0,
		&_vcam),
		"Failed to create virtual camera");

	WINTRACE(L"RegisterVirtualCamera '%s' ok", clsid.c_str());
	RETURN_IF_FAILED_MSG(_vcam->Start(nullptr), "Cannot start VCam");
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
	const auto status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kGstConfigPath, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
	RETURN_HR_IF(HRESULT_FROM_WIN32(status), status != ERROR_SUCCESS);

	auto setStatus = RegSetValueExW(key, kPipelineValueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(pipeline.c_str()), static_cast<DWORD>((pipeline.size() + 1) * sizeof(wchar_t)));
	if (setStatus == ERROR_SUCCESS)
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
	RETURN_HR_IF(HRESULT_FROM_WIN32(setStatus), setStatus != ERROR_SUCCESS);
	return S_OK;
}

HRESULT UnregisterVirtualCamera()
{
	if (!_vcam)
	{
		return S_OK;
	}

	const auto hr = _vcam->Remove();
	WINTRACE(L"Remove VCam hr:0x%08X", hr);
	_vcam.reset();
	return S_OK;
}

