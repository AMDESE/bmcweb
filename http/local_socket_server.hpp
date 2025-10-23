#pragma once
#include "http_connection.hpp"
#include "logging.hpp"

#include <systemd/sd-daemon.h>

#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <ctime>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace crow
{

template <typename Handler,
          typename Adaptor = boost::asio::local::stream_protocol::socket>
class LocalSocketServer
{
    using Acceptor = boost::asio::local::stream_protocol::acceptor;
    using self_t = LocalSocketServer<Handler, Adaptor>;

  public:
    LocalSocketServer(Handler* handlerIn, Acceptor&& acceptorIn,
                      boost::asio::io_context& ioContextIn) :
        handler(handlerIn), localAcceptor(std::move(acceptorIn)),
        ioContext(ioContextIn)
    {}

    void run()
    {
        BMCWEB_LOG_INFO("Local socket server is running");
        doAccept();
    }

    void doAccept()
    {
        auto socket = std::make_unique<Adaptor>(ioContext);
        Adaptor* socketPtr = socket.get();

        localAcceptor.async_accept(
            *socketPtr,
            std::bind_front(&self_t::afterAccept, this, std::move(socket)));
    }

    void afterAccept(std::unique_ptr<Adaptor> socket,
                     const boost::system::error_code& ec)
    {
        if (ec)
        {
            BMCWEB_LOG_ERROR("Failed to accept local socket connection {}", ec);
            return;
        }

        BMCWEB_LOG_DEBUG("Accepted local socket connection");

        // Create a dummy SSL context for local socket connections
        // Local sockets don't need SSL, but the Connection class expects it
        auto sslCtx = std::make_shared<boost::asio::ssl::context>(
            boost::asio::ssl::context::tls_server);

        // Create a dummy SSL stream that wraps the local socket
        boost::asio::ssl::stream<Adaptor> sslStream(std::move(*socket),
                                                    *sslCtx);

        // Create a timer for the connection
        boost::asio::steady_timer timer(ioContext);

        // Create connection with admin privileges
        using ConnectionType = Connection<Adaptor, Handler>;
        auto connection = std::make_shared<ConnectionType>(
            handler, HttpType::HTTP, std::move(timer), getCachedDateStr,
            std::move(sslStream));

        // Disable authentication for local socket connections
        // This allows them to bypass all authentication checks
        connection->disableAuth();

        // Start the connection - it will detect that it's not SSL and proceed
        // with HTTP
        boost::asio::post(ioContext, [connection] { connection->start(); });

        // Continue accepting new connections
        doAccept();
    }

    static std::optional<Acceptor> setupLocalSocket(
        boost::asio::io_context& ioContext)
    {
        using boost::asio::local::stream_protocol;
        char** names = nullptr;
        int listenFdCount = sd_listen_fds_with_names(0, &names);
        BMCWEB_LOG_DEBUG("Got {} sockets to open", listenFdCount);

        if (listenFdCount < 0)
        {
            BMCWEB_LOG_CRITICAL("Failed to read socket files");
            return std::nullopt;
        }
        int socketIndex = 0;
        for (char* name :
             std::span<char*>(names, static_cast<size_t>(listenFdCount)))
        {
            if (name == nullptr)
            {
                continue;
            }

            int listenFd = socketIndex + SD_LISTEN_FDS_START;
            if (sd_is_socket_unix(listenFd, SOCK_STREAM, 1, nullptr, 0) > 0)
            {
                BMCWEB_LOG_INFO("Starting webserver on local socket handle {}",
                                listenFd);
                return stream_protocol::acceptor(ioContext, stream_protocol(),
                                                 listenFd);
            }
            socketIndex++;
        }

        constexpr const char* socketPath = "/tmp/run/local_access.sock";
        BMCWEB_LOG_INFO("Starting webserver on local socket {}", socketPath);
        stream_protocol::endpoint end(socketPath);
        return stream_protocol::acceptor(ioContext, end);
    }

  private:
    Handler* handler;
    Acceptor localAcceptor;
    boost::asio::io_context& ioContext;
    std::string dateStr;

    void updateDateStr()
    {
        time_t lastTimeT = time(nullptr);
        tm myTm{};

        gmtime_r(&lastTimeT, &myTm);

        dateStr.resize(100);
        size_t dateStrSz = strftime(dateStr.data(), dateStr.size() - 1,
                                    "%a, %d %b %Y %H:%M:%S GMT", &myTm);
        dateStr.resize(dateStrSz);
    }

    std::function<std::string()> getCachedDateStr = [this]() -> std::string {
        static std::chrono::time_point<std::chrono::steady_clock>
            lastDateUpdate = std::chrono::steady_clock::now();
        if (std::chrono::steady_clock::now() - lastDateUpdate >=
            std::chrono::seconds(10))
        {
            lastDateUpdate = std::chrono::steady_clock::now();
            updateDateStr();
        }
        return this->dateStr;
    };
};

} // namespace crow
