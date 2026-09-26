#pragma once

#include <winsock2.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include "../protocol/Protocol.h"

namespace dd::transport
{

class WinsockScope
{
  public:
    WinsockScope();
    ~WinsockScope();
    WinsockScope(const WinsockScope&) = delete;
    WinsockScope& operator=(const WinsockScope&) = delete;
    bool Ok() const { return m_ok; }

  private:
    bool m_ok = false;
};

class Connection
{
  public:
    explicit Connection(SOCKET socket);
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // Thread-safe. Header and payload go out in a single gathered send.
    bool Send(protocol::MessageType type, uint8_t flags, uint64_t timestamp, std::span<const uint8_t> payload);

    // Blocking; call from one reader thread. Returns false on disconnect or protocol error.
    bool Receive(protocol::Header& header, std::vector<uint8_t>& payload);

    // Unblocks a pending Receive() from another thread.
    void Shutdown();

  private:
    bool ReadExact(uint8_t* data, size_t size);

    SOCKET m_socket;
    std::mutex m_sendMutex;
};

class Listener
{
  public:
    Listener() = default;
    ~Listener();
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    // Binds to 127.0.0.1 only: the protocol is unauthenticated (docs/wire-protocol.md).
    bool Listen(uint16_t port);
    std::unique_ptr<Connection> Accept();
    void Close();

  private:
    SOCKET m_socket = INVALID_SOCKET;
};

} // namespace dd::transport
