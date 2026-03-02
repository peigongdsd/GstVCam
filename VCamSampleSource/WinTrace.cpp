#include "pch.h"
#include "Tools.h"
#include "TcpKick.h"

#include <mutex>
#include <string>

namespace
{
	static constexpr PCWSTR kTraceFilePath = L"C:\\gstvcam_trace.txt";
	static std::mutex g_traceLock;
	static bool g_traceEnabled = false;

	std::wstring TrimLineEnding(std::wstring text)
	{
		while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n'))
		{
			text.pop_back();
		}
		return text;
	}

	std::wstring BuildTimestamp()
	{
		SYSTEMTIME st{};
		GetLocalTime(&st);
		return std::format(
			L"{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}",
			st.wYear,
			st.wMonth,
			st.wDay,
			st.wHour,
			st.wMinute,
			st.wSecond,
			st.wMilliseconds);
	}

	void WriteUtf8LineToFileAndFlush(const std::string& utf8Line)
	{
		HANDLE file = CreateFileW(
			kTraceFilePath,
			FILE_APPEND_DATA,
			FILE_SHARE_READ | FILE_SHARE_WRITE,
			nullptr,
			OPEN_ALWAYS,
			FILE_ATTRIBUTE_NORMAL,
			nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			return;
		}

		DWORD written = 0;
		WriteFile(file, utf8Line.data(), static_cast<DWORD>(utf8Line.size()), &written, nullptr);
		FlushFileBuffers(file);
		CloseHandle(file);
	}

	void EmitLine(const std::wstring& rawMessage)
	{
		auto msg = TrimLineEnding(rawMessage);
		auto line = std::format(L"[{}]{}", BuildTimestamp(), msg);
		const auto utf8Bytes = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, nullptr, 0, nullptr, nullptr);
		if (utf8Bytes <= 1)
		{
			return;
		}

		std::string utf8(static_cast<size_t>(utf8Bytes), '\0');
		if (WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, utf8.data(), utf8Bytes, nullptr, nullptr) <= 0)
		{
			return;
		}
		utf8.pop_back(); // drop terminal null
		utf8 += "\r\n";

		std::lock_guard<std::mutex> lock(g_traceLock);
		WriteUtf8LineToFileAndFlush(utf8);
		(void)TcpKickSendLogUtf8(utf8.data(), utf8.size());
	}
}

HRESULT GetTraceId(GUID* pGuid)
{
	if (!pGuid)
	{
		return E_INVALIDARG;
	}

	*pGuid = GUID_NULL;
	return S_OK;
}

ULONG WinTraceRegister()
{
	std::lock_guard<std::mutex> lock(g_traceLock);
	g_traceEnabled = true;
	return ERROR_SUCCESS;
}

void WinTraceUnregister()
{
	std::lock_guard<std::mutex> lock(g_traceLock);
	g_traceEnabled = false;
}

void WinTraceFormat(UCHAR level, ULONGLONG keyword, PCWSTR format, ...)
{
	UNREFERENCED_PARAMETER(level);
	UNREFERENCED_PARAMETER(keyword);

	if (!format)
	{
		return;
	}

	{
		std::lock_guard<std::mutex> lock(g_traceLock);
		if (!g_traceEnabled)
		{
			return;
		}
	}

	WCHAR buffer[2048]{};
	va_list args;
	va_start(args, format);
	StringCchVPrintfW(buffer, _countof(buffer), format, args);
	va_end(args);

	EmitLine(buffer);
}

void WinTraceFormat(UCHAR level, ULONGLONG keyword, PCSTR format, ...)
{
	UNREFERENCED_PARAMETER(level);
	UNREFERENCED_PARAMETER(keyword);

	if (!format)
	{
		return;
	}

	{
		std::lock_guard<std::mutex> lock(g_traceLock);
		if (!g_traceEnabled)
		{
			return;
		}
	}

	CHAR buffer[2048]{};
	va_list args;
	va_start(args, format);
	StringCchVPrintfA(buffer, _countof(buffer), format, args);
	va_end(args);

	EmitLine(to_wstring(buffer));
}

void WinTrace(UCHAR level, ULONGLONG keyword, PCWSTR string)
{
	UNREFERENCED_PARAMETER(level);
	UNREFERENCED_PARAMETER(keyword);

	if (!string)
	{
		return;
	}

	{
		std::lock_guard<std::mutex> lock(g_traceLock);
		if (!g_traceEnabled)
		{
			return;
		}
	}

	EmitLine(string);
}

void WinTrace(UCHAR level, ULONGLONG keyword, PCSTR string)
{
	UNREFERENCED_PARAMETER(level);
	UNREFERENCED_PARAMETER(keyword);

	if (!string)
	{
		return;
	}

	{
		std::lock_guard<std::mutex> lock(g_traceLock);
		if (!g_traceEnabled)
		{
			return;
		}
	}

	EmitLine(to_wstring(string));
}
