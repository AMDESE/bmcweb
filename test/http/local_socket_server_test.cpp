// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors
#include "async_resp.hpp"
#include "http/http_request.hpp"
#include "http/local_socket_server.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/write.hpp>

#include <memory>
#include <thread>

#include "gtest/gtest.h"

namespace crow
{

// Enhanced mock handler for testing - captures request data
struct MockHandler
{
    void handle(const std::shared_ptr<crow::Request>& req,
                const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)
    {
        handleCalled = true;

        EXPECT_EQ(req->method(), boost::beast::http::verb::post);
        EXPECT_EQ(req->target(), "/test/endpoint");
        EXPECT_EQ(req->getHeaderValue(boost::beast::http::field::host),
                  "localServer");
        EXPECT_EQ(req->body(), R"({"test": "data", "value": 42})");

        // Send a simple response
        if (asyncResp)
        {
            asyncResp->res.result(boost::beast::http::status::ok);
            asyncResp->res.jsonValue["message"] =
                "Request received successfully";
        }
    }

    template <typename Adaptor>
    void handleUpgrade(const std::shared_ptr<crow::Request>& /*req*/,
                       const std::shared_ptr<bmcweb::AsyncResp>& /*asyncResp*/,
                       Adaptor&& /*adaptor*/)
    {
        // Local socket connections don't use upgrade
        EXPECT_FALSE(true);
    }

    bool handleCalled = false;
};

TEST(local_socket_server, ReceiveDataFromLocalSocket)
{
    using Acceptor = boost::asio::local::stream_protocol::acceptor;
    using Socket = boost::asio::local::stream_protocol::socket;
    using Endpoint = boost::asio::local::stream_protocol::endpoint;
    boost::asio::io_context io;

    // Create a temporary socket file for testing
    const std::string socketPath = "/tmp/test_local_socket.sock";
    std::remove(socketPath.c_str()); // Clean up any existing socket file

    // Create acceptor bound to the socket file
    Endpoint endpoint(socketPath);
    Acceptor acceptor(io, endpoint);
    MockHandler handler;

    LocalSocketServer<MockHandler> server(&handler, std::move(acceptor), io);

    // Start the server in a separate thread
    std::thread serverThread(
        &server, &io {
            server.run();
            // Run the io_context to process async operations
            io.run();
        });

    // Give the server time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Create a client socket and connect
    Socket clientSocket(io);
    boost::system::error_code ec;
    clientSocket.connect(endpoint, ec);

    ASSERT_FALSE(ec) << "Failed to connect to local socket: " << ec.message();

    // Prepare HTTP request data
    boost::beast::http::request<boost::beast::http::string_body> req{
        boost::beast::http::verb::post, "/test/endpoint", 11};
    req.set(boost::beast::http::field::host, "localServer");
    req.set(boost::beast::http::field::content_type, "application/json");
    req.set(boost::beast::http::field::user_agent, "TestClient/1.0");
    req.set(boost::beast::http::field::content_length, "32");
    req.body() = R"({"test": "data", "value": 42})";
    req.prepare_payload();

    // Send the HTTP request
    boost::beast::http::write(clientSocket, req, ec);
    ASSERT_FALSE(ec) << "Failed to write HTTP request: " << ec.message();

    // Wait for the handler to be called
    auto start = std::chrono::steady_clock::now();
    while (!handler.handleCalled &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(5))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Verify that the handler was called and data was captured correctly
    EXPECT_TRUE(handler.handleCalled) << "Handler was not called";

    // Give some time for the response to be sent
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Read and verify the HTTP response
    boost::beast::flat_buffer buffer;
    boost::beast::http::response<boost::beast::http::string_body> response;
    boost::beast::http::read(clientSocket, buffer, response, ec);
    ASSERT_FALSE(ec) << "Failed to read HTTP response: " << ec.message();

    // Verify response status
    EXPECT_EQ(response.result(), boost::beast::http::status::ok);
    EXPECT_EQ(response.result_int(), 200);

    // Verify response headers
    EXPECT_EQ(response[boost::beast::http::field::content_type],
              "application/json");

    // Verify response body contains the expected JSON message
    EXPECT_FALSE(response.body().empty());
    EXPECT_TRUE(response.body().find("Request received successfully") !=
                std::string::npos);
    EXPECT_TRUE(response.body().find("\"message\"") != std::string::npos);

    // Clean up
    clientSocket.close();

    // Stop the io_context to allow the server thread to exit
    io.stop();
    serverThread.join();
    std::remove(socketPath.c_str());
}

} // namespace crow
