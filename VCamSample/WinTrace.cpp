#include "framework.h"
#include "Tools.h"
#include <mutex>
#include <vector>

// we don't use OutputDebugString because it's 100% crap, truncating, slow, etc.
// use WpfTraceSpy https://github.com/smourier/TraceSpy to see these traces (configure an ETW Provider with guid set to 964d4572-adb9-4f3a-8170-fcbecec27467)
static GUID GUID_WinTraceProvider = { 0x964d4572,0xadb9,0x4f3a,{0x81,0x70,0xfc,0xbe,0xce,0xc2,0x74,0x67} };
static constexpr PCWSTR kTraceFilePath = L"C:\\vcamsample_trace.txt";

REGHANDLE _traceHandle = 0;

static void AppendTraceToFile(PCWSTR text)
{
	if (!text || !*text)
		return;

	static std::mutex s_fileLock;
	std::lock_guard<std::mutex> guard(s_fileLock);

	HANDLE file = CreateFileW(
		kTraceFilePath,
		FILE_APPEND_DATA,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr,
		OPEN_ALWAYS,
		FILE_ATTRIBUTE_NORMAL,
		nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return;

	auto line = std::format(L"{}\r\n", text);
	const auto utf8Bytes = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, nullptr, 0, nullptr, nullptr);
	if (utf8Bytes > 1)
	{
		std::vector<char> buffer(static_cast<size_t>(utf8Bytes));
		if (WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, buffer.data(), utf8Bytes, nullptr, nullptr) > 0)
		{
			DWORD written = 0;
			WriteFile(file, buffer.data(), static_cast<DWORD>(utf8Bytes - 1), &written, nullptr);
		}
	}

	CloseHandle(file);
}

static void AppendTraceToConsole(PCWSTR text)
{
	if (!text || !*text)
		return;

	HANDLE handle = GetStdHandle(STD_ERROR_HANDLE);
	if (!handle || handle == INVALID_HANDLE_VALUE)
	{
		handle = GetStdHandle(STD_OUTPUT_HANDLE);
	}
	if (!handle || handle == INVALID_HANDLE_VALUE)
		return;

	auto line = std::format(L"{}\r\n", text);

	DWORD mode = 0;
	if (GetConsoleMode(handle, &mode))
	{
		DWORD written = 0;
		WriteConsoleW(handle, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
		return;
	}

	const auto utf8Bytes = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, nullptr, 0, nullptr, nullptr);
	if (utf8Bytes > 1)
	{
		std::vector<char> buffer(static_cast<size_t>(utf8Bytes));
		if (WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, buffer.data(), utf8Bytes, nullptr, nullptr) > 0)
		{
			DWORD written = 0;
			WriteFile(handle, buffer.data(), static_cast<DWORD>(utf8Bytes - 1), &written, nullptr);
		}
	}
}

HRESULT GetTraceId(GUID* pGuid)
{
	if (!pGuid)
		return E_INVALIDARG;

	*pGuid = GUID_WinTraceProvider;
	return S_OK;
}

ULONG WinTraceRegister()
{
	return EventRegister(&GUID_WinTraceProvider, nullptr, nullptr, &_traceHandle);
}

void WinTraceUnregister()
{
	auto h = _traceHandle;
	if (h)
	{
		_traceHandle = 0;
		EventUnregister(h);
	}
}

void WinTraceFormat(UCHAR level, ULONGLONG keyword, PCWSTR format, ...)
{
	if (!_traceHandle)
		return;

	WCHAR szTrace[2048];
	va_list args;
	va_start(args, format);
	// add '00000000:' before all traces
	StringCchPrintf(szTrace, (size_t)(9 + 1), L"%08X:", GetCurrentThreadId());
	StringCchVPrintfW(((LPWSTR)szTrace) + 9, _countof(szTrace) - 10, format, args);
	va_end(args);
	EventWriteString(_traceHandle, level, keyword, szTrace);
	AppendTraceToFile(szTrace);
	AppendTraceToConsole(szTrace);
}

void WinTraceFormat(UCHAR level, ULONGLONG keyword, PCSTR format, ...)
{
	if (!_traceHandle)
		return;

	CHAR szTrace[2048];
	va_list args;
	va_start(args, format);
	StringCchPrintfA(szTrace, (size_t)(9 + 1), "%08X:", GetCurrentThreadId());
	StringCchVPrintfA(((LPSTR)szTrace) + 9, _countof(szTrace) - 10, format, args);
	va_end(args);
	auto ws = to_wstring(szTrace);
	EventWriteString(_traceHandle, level, keyword, ws.c_str());
	AppendTraceToFile(ws.c_str());
	AppendTraceToConsole(ws.c_str());
}

void WinTrace(UCHAR level, ULONGLONG keyword, PCSTR string)
{
	if (!_traceHandle)
		return;

	auto ws = to_wstring(string);
	EventWriteString(_traceHandle, level, keyword, ws.c_str());
	AppendTraceToFile(ws.c_str());
	AppendTraceToConsole(ws.c_str());
}

void WinTrace(UCHAR level, ULONGLONG keyword, PCWSTR string)
{
	if (!_traceHandle)
		return;

	EventWriteString(_traceHandle, level, keyword, string);
	AppendTraceToFile(string);
	AppendTraceToConsole(string);
}
