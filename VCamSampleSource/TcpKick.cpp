#include "pch.h"
#include "Tools.h"

#include <climits>
#include <mutex>
#include <string>

#pragma comment(lib, "ws2_32.lib")

namespace
{
	constexpr PCWSTR kConfigPath = L"SOFTWARE\\VCamSample\\GStreamer";
	constexpr PCWSTR kLogEndpointValueName = L"LogEndpoint";
	constexpr PCWSTR kDefaultLogEndpoint = L"tcp://192.168.122.1:5555";

	std::mutex g_lock;
	SOCKET g_socket = INVALID_SOCKET;
	bool g_wsaInitialized = false;

	std::wstring ReadLogEndpoint()
	{
		HKEY key = nullptr;
		if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kConfigPath, 0, KEY_READ, &key) != ERROR_SUCCESS)
		{
			return kDefaultLogEndpoint;
		}

		wchar_t endpoint[512]{};
		DWORD type = 0;
		DWORD size = sizeof(endpoint);
		if (RegGetValueW(key, nullptr, kLogEndpointValueName, RRF_RT_REG_SZ, &type, endpoint, &size) != ERROR_SUCCESS)
		{
			RegCloseKey(key);
			return kDefaultLogEndpoint;
		}

		RegCloseKey(key);
		return endpoint;
	}

	bool ParseTcpEndpoint(const std::wstring& endpoint, std::string* host, std::string* port)
	{
		if (!host || !port)
			return false;

		constexpr PCWSTR prefix = L"tcp://";
		constexpr size_t prefixLen = 6;
		if (endpoint.size() <= prefixLen || endpoint.rfind(prefix, 0) != 0)
			return false;

		auto hostPort = endpoint.substr(prefixLen);
		auto colon = hostPort.rfind(L':');
		if (colon == std::wstring::npos || colon == 0 || colon + 1 >= hostPort.size())
			return false;

		*host = to_string(hostPort.substr(0, colon));
		*port = to_string(hostPort.substr(colon + 1));
		return !host->empty() && !port->empty();
	}

	void CloseSocket_NoLock()
	{
		if (g_socket != INVALID_SOCKET)
		{
			closesocket(g_socket);
			g_socket = INVALID_SOCKET;
		}
	}

	HRESULT EnsureWsa_NoLock()
	{
		if (g_wsaInitialized)
			return S_OK;

		WSADATA wsaData{};
		const auto wsaStatus = WSAStartup(MAKEWORD(2, 2), &wsaData);
		RETURN_HR_IF(HRESULT_FROM_WIN32(wsaStatus), wsaStatus != 0);
		g_wsaInitialized = true;
		return S_OK;
	}
}

HRESULT TcpKickConnectFromRegistry()
{
	std::lock_guard<std::mutex> guard(g_lock);
	if (g_socket != INVALID_SOCKET)
	{
		return S_OK;
	}

	std::string host;
	std::string port;
	const auto endpoint = ReadLogEndpoint();
	RETURN_HR_IF(E_INVALIDARG, !ParseTcpEndpoint(endpoint, &host, &port));

	RETURN_IF_FAILED(EnsureWsa_NoLock());

	addrinfo hints{};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	addrinfo* result = nullptr;
	if (getaddrinfo(host.c_str(), port.c_str(), &hints, &result) != 0)
	{
		return HRESULT_FROM_WIN32(WSAGetLastError());
	}

	HRESULT hr = HRESULT_FROM_WIN32(WSAECONNREFUSED);
	for (addrinfo* ptr = result; ptr; ptr = ptr->ai_next)
	{
		SOCKET sock = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
		if (sock == INVALID_SOCKET)
		{
			hr = HRESULT_FROM_WIN32(WSAGetLastError());
			continue;
		}

		if (connect(sock, ptr->ai_addr, static_cast<int>(ptr->ai_addrlen)) == 0)
		{
			g_socket = sock;
			hr = S_OK;
			break;
		}

		hr = HRESULT_FROM_WIN32(WSAGetLastError());
		closesocket(sock);
	}

	freeaddrinfo(result);
	return hr;
}

void TcpKickDisconnect()
{
	std::lock_guard<std::mutex> guard(g_lock);
	CloseSocket_NoLock();

	if (g_wsaInitialized)
	{
		WSACleanup();
		g_wsaInitialized = false;
	}
}

bool TcpKickSendLogUtf8(const char* data, size_t size)
{
	if (!data || size == 0)
	{
		return false;
	}

	std::lock_guard<std::mutex> guard(g_lock);
	if (g_socket == INVALID_SOCKET)
	{
		return false;
	}

	size_t totalSent = 0;
	while (totalSent < size)
	{
		const auto remaining = size - totalSent;
		const auto maxInt = static_cast<size_t>(INT_MAX);
		const auto chunk = static_cast<int>((remaining > maxInt) ? maxInt : remaining);
		const auto sent = send(g_socket, data + totalSent, chunk, 0);
		if (sent <= 0)
		{
			return false;
		}
		totalSent += static_cast<size_t>(sent);
	}
	return true;
}
