#include "Tcp.h"

#include <ws2tcpip.h>

#include <array>

#include "../common/Log.h"

namespace dd::transport
{

WinsockScope::WinsockScope()
{
    WSADATA data{};
    m_ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

WinsockScope::~WinsockScope()
{
    if (m_ok)
    {
        WSACleanup();
    }
}

Connection::Connection(SOCKET socket) : m_socket(socket)
{
    BOOL noDelay = TRUE;
    setsockopt(m_socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
}

Connection::~Connection()
{
    if (m_qos)
    {
        if (m_qosFlow)
        {
            QOSRemoveSocketFromFlow(m_qos, m_socket, m_qosFlow, 0);
        }
        QOSCloseHandle(m_qos);
    }
    closesocket(m_socket);
}

void Connection::SetReceiveTimeout(DWORD ms)
{
    setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
}

void Connection::EnableWifiQos()
{
    QOS_VERSION version{1, 0};
    if (!m_qos && !QOSCreateHandle(&version, &m_qos))
    {
        m_qos = nullptr;
        Log(L"wifi: QOSCreateHandle failed (%lu)", GetLastError());
        return;
    }
    if (!QOSAddSocketToFlow(m_qos, m_socket, nullptr, QOSTrafficTypeAudioVideo, QOS_NON_ADAPTIVE_FLOW, &m_qosFlow))
    {
        m_qosFlow = 0;
        Log(L"wifi: QoS tagging unavailable (%lu), sending best effort", GetLastError());
    }
}

bool Connection::Send(protocol::MessageType type, uint8_t flags, uint64_t timestamp,
                      std::span<const uint8_t> payload)
{
    std::array<uint8_t, protocol::kHeaderSize> header{};
    protocol::EncodeHeader({type, flags, static_cast<uint32_t>(payload.size()), timestamp}, header);

    WSABUF buffers[2] = {
        {static_cast<ULONG>(header.size()), reinterpret_cast<CHAR*>(header.data())},
        {static_cast<ULONG>(payload.size()), const_cast<CHAR*>(reinterpret_cast<const CHAR*>(payload.data()))},
    };
    DWORD bufferCount = payload.empty() ? 1 : 2;
    size_t remaining = header.size() + payload.size();

    std::lock_guard lock(m_sendMutex);
    WSABUF* current = buffers;
    while (remaining > 0)
    {
        DWORD sent = 0;
        if (WSASend(m_socket, current, bufferCount, &sent, 0, nullptr, nullptr) == SOCKET_ERROR)
        {
            return false;
        }
        remaining -= sent;
        // Partial send: advance through the buffer list.
        while (bufferCount > 0 && sent >= current->len)
        {
            sent -= current->len;
            ++current;
            --bufferCount;
        }
        if (bufferCount > 0)
        {
            current->buf += sent;
            current->len -= sent;
        }
    }
    return true;
}

bool Connection::ReadExact(uint8_t* data, size_t size)
{
    while (size > 0)
    {
        const int chunk = static_cast<int>(size > 1 << 20 ? 1 << 20 : size);
        const int got = recv(m_socket, reinterpret_cast<char*>(data), chunk, 0);
        if (got <= 0)
        {
            return false;
        }
        data += got;
        size -= static_cast<size_t>(got);
    }
    return true;
}

bool Connection::Receive(protocol::Header& header, std::vector<uint8_t>& payload)
{
    std::array<uint8_t, protocol::kHeaderSize> raw{};
    if (!ReadExact(raw.data(), raw.size()))
    {
        return false;
    }
    auto decoded = protocol::DecodeHeader(raw);
    if (!decoded)
    {
        return false;
    }
    header = *decoded;
    payload.resize(header.length);
    return header.length == 0 || ReadExact(payload.data(), payload.size());
}

void Connection::Shutdown()
{
    shutdown(m_socket, SD_BOTH);
}

Listener::~Listener()
{
    Close();
}

bool Listener::Listen(uint16_t port)
{
    in_addr loopback{};
    inet_pton(AF_INET, "127.0.0.1", &loopback);
    return Listen(port, loopback);
}

bool Listener::Listen(uint16_t port, const in_addr& address)
{
    m_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_socket == INVALID_SOCKET)
    {
        return false;
    }

    BOOL exclusive = TRUE;
    setsockopt(m_socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
               sizeof(exclusive));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr = address;

    if (bind(m_socket, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        listen(m_socket, 1) == SOCKET_ERROR)
    {
        Close();
        return false;
    }
    return true;
}

std::unique_ptr<Connection> Listener::Accept()
{
    SOCKET client = accept(m_socket, nullptr, nullptr);
    if (client == INVALID_SOCKET)
    {
        return nullptr;
    }
    return std::make_unique<Connection>(client);
}

void Listener::Close()
{
    if (m_socket != INVALID_SOCKET)
    {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }
}

} // namespace dd::transport
