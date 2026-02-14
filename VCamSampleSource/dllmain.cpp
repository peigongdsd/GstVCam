#include "pch.h"
#include "Undocumented.h"
#include "Tools.h"
#include "EnumNames.h"
#include "Activator.h"
#include <cwctype>

// 3cad447d-f283-4af4-a3b2-6f5363309f52
GUID CLSID_VCam = { 0x3cad447d,0xf283,0x4af4,{0xa3,0xb2,0x6f,0x53,0x63,0x30,0x9f,0x52} };
HMODULE _hModule;
using registry_key = winrt::handle_type<registry_traits>;
constexpr PCWSTR kVirtualCameraName = L"VCamSample";
constexpr PCWSTR kGstConfigPath = L"SOFTWARE\\VCamSample\\GStreamer";
constexpr PCWSTR kPipelineValueName = L"Pipeline";
constexpr PCWSTR kBinPathValueName = L"BinPath";
constexpr PCWSTR kWidthValueName = L"Width";
constexpr PCWSTR kHeightValueName = L"Height";
constexpr PCWSTR kFpsNumValueName = L"FpsNumerator";
constexpr PCWSTR kFpsDenValueName = L"FpsDenominator";
constexpr DWORD kDefaultWidth = 1280;
constexpr DWORD kDefaultHeight = 960;
constexpr DWORD kDefaultFpsNum = 30;
constexpr DWORD kDefaultFpsDen = 1;

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

std::wstring BuildDefaultPipeline(DWORD width, DWORD height, DWORD fpsNum, DWORD fpsDen)
{
	return std::format(
		L"videotestsrc is-live=true pattern=smpte ! video/x-raw,format=NV12,width={},height={},framerate={}/{} ! appsink name=vcamsink",
		width,
		height,
		fpsNum,
		fpsDen);
}

std::wstring ToLower(std::wstring value)
{
	for (auto& ch : value)
	{
		ch = static_cast<wchar_t>(towlower(ch));
	}
	return value;
}

std::wstring ResolvePipelineOverride(PCWSTR input)
{
	auto value = Trim(input ? input : L"");
	if (value.empty())
	{
		return {};
	}

	const auto lowerValue = ToLower(value);
	constexpr PCWSTR kPrefix = L"pipeline=";
	if (lowerValue.rfind(kPrefix, 0) == 0)
	{
		return Trim(value.substr(wcslen(kPrefix)));
	}

	// Accept raw pipeline text only if it clearly looks like a GStreamer pipeline.
	if (value.find(L'!') != std::wstring::npos)
	{
		return value;
	}
	return {};
}

bool TryReadDwordValue(HKEY key, PCWSTR valueName, DWORD* outValue)
{
	DWORD value = 0;
	DWORD size = sizeof(value);
	if (RegGetValueW(key, nullptr, valueName, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS)
	{
		*outValue = value;
		return true;
	}
	return false;
}

bool TryReadStringValue(HKEY key, PCWSTR valueName, std::wstring* outValue)
{
	if (!outValue)
	{
		return false;
	}
	wchar_t buffer[4096]{};
	DWORD size = sizeof(buffer);
	if (RegGetValueW(key, nullptr, valueName, RRF_RT_REG_SZ, nullptr, buffer, &size) == ERROR_SUCCESS)
	{
		*outValue = buffer;
		return true;
	}
	return false;
}

bool IsExistingFile(const std::wstring& path)
{
	const auto attributes = GetFileAttributesW(path.c_str());
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool IsValidGStreamerBinPath(const std::wstring& binPath)
{
	if (binPath.empty())
	{
		return false;
	}

	const auto prefix = binPath.back() == L'\\' ? binPath : binPath + L"\\";
	return IsExistingFile(prefix + L"gstreamer-1.0-0.dll") &&
		IsExistingFile(prefix + L"gstapp-1.0-0.dll") &&
		IsExistingFile(prefix + L"gstvideo-1.0-0.dll");
}

std::wstring FindDefaultGStreamerBinPath()
{
	const std::wstring preferred = L"C:\\Program Files\\gstreamer\\1.0\\msvc_x86_64\\bin";
	if (IsValidGStreamerBinPath(preferred))
	{
		return preferred;
	}

	const std::wstring roots[] =
	{
		L"C:\\Program Files\\gstreamer\\1.0\\",
		L"C:\\Program Files (x86)\\gstreamer\\1.0\\"
	};

	for (const auto& root : roots)
	{
		WIN32_FIND_DATAW findData{};
		const auto searchPattern = root + L"msvc_*";
		auto findHandle = FindFirstFileW(searchPattern.c_str(), &findData);
		if (findHandle == INVALID_HANDLE_VALUE)
		{
			continue;
		}

		do
		{
			if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
			{
				continue;
			}
			if (wcscmp(findData.cFileName, L".") == 0 || wcscmp(findData.cFileName, L"..") == 0)
			{
				continue;
			}

			const auto candidate = root + findData.cFileName + L"\\bin";
			if (IsValidGStreamerBinPath(candidate))
			{
				FindClose(findHandle);
				return candidate;
			}
		} while (FindNextFileW(findHandle, &findData));

		FindClose(findHandle);
	}

	return {};
}

HRESULT SetDwordValue(HKEY key, PCWSTR valueName, DWORD value)
{
	return HRESULT_FROM_WIN32(RegSetValueExW(key, valueName, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value)));
}

HRESULT SetStringValue(HKEY key, PCWSTR valueName, const std::wstring& value)
{
	return HRESULT_FROM_WIN32(RegSetValueExW(key, valueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))));
}

HRESULT EnsurePipelineRegistry(PCWSTR pipelineOverride)
{
	registry_key key;
	const auto openStatus = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kGstConfigPath, 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, key.put(), nullptr);
	RETURN_HR_IF(HRESULT_FROM_WIN32(openStatus), openStatus != ERROR_SUCCESS);

	DWORD width = kDefaultWidth;
	DWORD height = kDefaultHeight;
	DWORD fpsNum = kDefaultFpsNum;
	DWORD fpsDen = kDefaultFpsDen;
	std::wstring pipeline;

	bool hasWidth = TryReadDwordValue(key.get(), kWidthValueName, &width) && width != 0;
	bool hasHeight = TryReadDwordValue(key.get(), kHeightValueName, &height) && height != 0;
	bool hasFpsNum = TryReadDwordValue(key.get(), kFpsNumValueName, &fpsNum) && fpsNum != 0;
	bool hasFpsDen = TryReadDwordValue(key.get(), kFpsDenValueName, &fpsDen) && fpsDen != 0;

	if (!hasWidth) width = kDefaultWidth;
	if (!hasHeight) height = kDefaultHeight;
	if (!hasFpsNum) fpsNum = kDefaultFpsNum;
	if (!hasFpsDen) fpsDen = kDefaultFpsDen;

	std::wstring binPath;
	const auto hasBinPath = TryReadStringValue(key.get(), kBinPathValueName, &binPath) && IsValidGStreamerBinPath(binPath);
	if (!hasBinPath)
	{
		binPath = FindDefaultGStreamerBinPath();
		if (!binPath.empty())
		{
			RETURN_IF_FAILED(SetStringValue(key.get(), kBinPathValueName, binPath));
		}
	}

	wchar_t pipelineBuffer[4096]{};
	DWORD pipelineSize = sizeof(pipelineBuffer);
	const auto pipelineReadStatus = RegGetValueW(key.get(), nullptr, kPipelineValueName, RRF_RT_REG_SZ, nullptr, pipelineBuffer, &pipelineSize);
	if (pipelineReadStatus == ERROR_SUCCESS)
	{
		pipeline = pipelineBuffer;
	}

	const auto overridePipeline = Trim(pipelineOverride ? pipelineOverride : L"");
	if (!overridePipeline.empty())
	{
		pipeline = overridePipeline;
	}

	if (pipeline.empty())
	{
		pipeline = BuildDefaultPipeline(width, height, fpsNum, fpsDen);
	}

	DWORD parsedWidth = 0;
	DWORD parsedHeight = 0;
	DWORD parsedFpsNum = 0;
	DWORD parsedFpsDen = 0;

	if (TryParseUIntToken(pipeline, L"width=", &parsedWidth) && parsedWidth)
	{
		width = parsedWidth;
	}
	if (TryParseUIntToken(pipeline, L"height=", &parsedHeight) && parsedHeight)
	{
		height = parsedHeight;
	}
	if (TryParseFramerateToken(pipeline, &parsedFpsNum, &parsedFpsDen) && parsedFpsNum && parsedFpsDen)
	{
		fpsNum = parsedFpsNum;
		fpsDen = parsedFpsDen;
	}

	RETURN_IF_FAILED(SetStringValue(key.get(), kPipelineValueName, pipeline));
	RETURN_IF_FAILED(SetDwordValue(key.get(), kWidthValueName, width));
	RETURN_IF_FAILED(SetDwordValue(key.get(), kHeightValueName, height));
	RETURN_IF_FAILED(SetDwordValue(key.get(), kFpsNumValueName, fpsNum));
	RETURN_IF_FAILED(SetDwordValue(key.get(), kFpsDenValueName, fpsDen));

	if (!binPath.empty())
	{
		WINTRACE(L"Pipeline registry configured width:%u height:%u fps:%u/%u bin:%s pipeline:%s", width, height, fpsNum, fpsDen, binPath.c_str(), pipeline.c_str());
	}
	else
	{
		WINTRACE(L"Pipeline registry configured width:%u height:%u fps:%u/%u bin:<not found> pipeline:%s", width, height, fpsNum, fpsDen, pipeline.c_str());
	}
	return S_OK;
}

HRESULT ConfigureVirtualCameraRegistration(bool install)
{
	wil::com_ptr_nothrow<IMFVirtualCamera> vcam;
	const auto clsid = GUID_ToStringW(CLSID_VCam, false);

	RETURN_IF_FAILED_MSG(MFCreateVirtualCamera(
		MFVirtualCameraType_SoftwareCameraSource,
		MFVirtualCameraLifetime_System,
		MFVirtualCameraAccess_CurrentUser,
		kVirtualCameraName,
		clsid.c_str(),
		nullptr,
		0,
		&vcam),
		"MFCreateVirtualCamera failed");

	if (install)
	{
		RETURN_IF_FAILED_MSG(vcam->Start(nullptr), "IMFVirtualCamera::Start failed");
		WINTRACE(L"DllInstall virtual camera provisioned clsid:%s", clsid.c_str());
		return S_OK;
	}

	const auto hr = vcam->Remove();
	if (SUCCEEDED(hr))
	{
		WINTRACE(L"DllInstall virtual camera removed clsid:%s", clsid.c_str());
	}
	else
	{
		WINTRACE(L"DllInstall virtual camera remove failed hr:0x%08X clsid:%s", hr, clsid.c_str());
	}
	return hr;
}

HRESULT RunVirtualCameraProvisioning(bool install, PCWSTR pipelineOverride)
{
	HRESULT hr = S_OK;
	auto coInitHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	auto shouldUninitializeCom = SUCCEEDED(coInitHr);
	if (coInitHr == RPC_E_CHANGED_MODE)
	{
		coInitHr = S_OK;
		shouldUninitializeCom = false;
	}
	if (FAILED(coInitHr))
	{
		return coInitHr;
	}

	auto mfInitHr = MFStartup(MF_VERSION);
	auto shouldShutdownMf = SUCCEEDED(mfInitHr);
	const auto pipelineText = ResolvePipelineOverride(pipelineOverride);
	if (FAILED(mfInitHr))
	{
		hr = mfInitHr;
		goto Cleanup;
	}

	if (install || !pipelineText.empty())
	{
		hr = EnsurePipelineRegistry(pipelineText.c_str());
		if (FAILED(hr))
		{
			goto Cleanup;
		}
	}

	hr = ConfigureVirtualCameraRegistration(install);

Cleanup:
	if (shouldShutdownMf)
	{
		MFShutdown();
	}
	if (shouldUninitializeCom)
	{
		CoUninitialize();
	}
	return hr;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved)
{
	switch (dwReason)
	{
	case DLL_PROCESS_ATTACH:
		_hModule = hModule;
		WinTraceRegister();
		WINTRACE(L"DllMain DLL_PROCESS_ATTACH '%s'", GetCommandLine());
		DisableThreadLibraryCalls(hModule);

		wil::SetResultLoggingCallback([](wil::FailureInfo const& failure) noexcept
			{
				wchar_t str[2048];
				if (SUCCEEDED(wil::GetFailureLogString(str, _countof(str), failure)))
				{
					WinTrace(2, 0, str); // 2 => error
				}
			});
		break;

	case DLL_PROCESS_DETACH:
		WINTRACE(L"DllMain DLL_PROCESS_DETACH '%s'", GetCommandLine());
		WinTraceUnregister();
		break;
	}
	return TRUE;
}

struct ClassFactory : winrt::implements<ClassFactory, IClassFactory>
{
	STDMETHODIMP CreateInstance(IUnknown* outer, GUID const& riid, void** result) noexcept final
	{
		RETURN_HR_IF_NULL(E_POINTER, result);
		*result = nullptr;
		if (outer)
			RETURN_HR(CLASS_E_NOAGGREGATION);

		auto vcam = winrt::make_self<Activator>();
		RETURN_IF_FAILED(vcam->Initialize());
		auto hr = vcam->QueryInterface(riid, result);
		if (FAILED(hr))
		{
			auto iid = GUID_ToStringW(riid);
			WINTRACE(L"ClassFactory QueryInterface failed on IID %s", iid.c_str());
		}
		return hr;
	}

	STDMETHODIMP LockServer(BOOL) noexcept final
	{
		return S_OK;
	}
};

__control_entrypoint(DllExport)
STDAPI DllCanUnloadNow()
{
	if (winrt::get_module_lock())
	{
		WINTRACE(L"DllCanUnloadNow S_FALSE");
		return S_FALSE;
	}

	winrt::clear_factory_cache();
	WINTRACE(L"DllCanUnloadNow S_OK");
	return S_OK;
}

_Check_return_
STDAPI DllGetClassObject(_In_ REFCLSID rclsid, _In_ REFIID riid, _Outptr_ LPVOID FAR* ppv)
{
	WINTRACE(L"DllGetClassObject rclsid:%s riid:%s", GUID_ToStringW(rclsid).c_str(), GUID_ToStringW(riid).c_str());
	RETURN_HR_IF_NULL(E_POINTER, ppv);
	*ppv = nullptr;

	if (rclsid == CLSID_VCam)
		return winrt::make_self<ClassFactory>()->QueryInterface(riid, ppv);

	RETURN_HR(E_NOINTERFACE);
}

STDAPI DllRegisterServer()
{
	std::wstring exePath = wil::GetModuleFileNameW(_hModule).get();
	WINTRACE(L"DllRegisterServer '%s'", exePath.c_str());
	auto clsid = GUID_ToStringW(CLSID_VCam, false);
	std::wstring path = L"Software\\Classes\\CLSID\\" + clsid + L"\\InprocServer32";

	// note: a vcam *must* be registered in HKEY_LOCAL_MACHINE
	// for the frame server to be able to talk with it.
	registry_key key;
	RETURN_IF_WIN32_ERROR(RegWriteKey(HKEY_LOCAL_MACHINE, path.c_str(), key.put()));
	RETURN_IF_WIN32_ERROR(RegWriteValue(key.get(), nullptr, exePath));
	RETURN_IF_WIN32_ERROR(RegWriteValue(key.get(), L"ThreadingModel", L"Both"));
	RETURN_IF_FAILED_MSG(RunVirtualCameraProvisioning(true, nullptr), "Virtual camera provisioning failed during DllRegisterServer");
	return S_OK;
}

STDAPI DllUnregisterServer()
{
	std::wstring exePath = wil::GetModuleFileNameW(_hModule).get();
	WINTRACE(L"DllUnregisterServer '%s'", exePath.c_str());
	const auto removeHr = RunVirtualCameraProvisioning(false, nullptr);
	if (FAILED(removeHr))
	{
		WINTRACE(L"Virtual camera remove during DllUnregisterServer failed hr:0x%08X", removeHr);
	}
	auto clsid = GUID_ToStringW(CLSID_VCam, false);
	std::wstring path = L"Software\\Classes\\CLSID\\" + clsid;
	RETURN_IF_WIN32_ERROR(RegDeleteTree(HKEY_LOCAL_MACHINE, path.c_str()));
	return S_OK;
}

STDAPI DllInstall(BOOL bInstall, PCWSTR pszCmdLine)
{
	WINTRACE(L"DllInstall install:%u cmd:%s", bInstall ? 1 : 0, pszCmdLine ? pszCmdLine : L"");
	return RunVirtualCameraProvisioning(bInstall != FALSE, pszCmdLine);
}
