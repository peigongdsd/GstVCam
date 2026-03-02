#pragma once
#include <cstddef>

HRESULT TcpKickConnectFromRegistry();
void TcpKickDisconnect();
bool TcpKickSendLogUtf8(const char* data, size_t size);
