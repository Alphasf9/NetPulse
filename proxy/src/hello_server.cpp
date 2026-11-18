#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <string>

#pragma comment(lib, "ws2_32.lib")

int main()
{
    // 1. Start Winsock (Windows's socket library)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }

    // 2. Create a socket: IPv4, TCP
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET)
    {
        std::cerr << "socket() failed: " << WSAGetLastError() << "\n";
        WSACleanup();
        return 1;
    }

    // 3. Attach the socket to port 8888 on all network interfaces
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(8888);

    if (bind(listener, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR)
    {
        std::cerr << "bind() failed: " << WSAGetLastError() << "\n";
        closesocket(listener);
        WSACleanup();
        return 1;
    }

    // 4. Start listening for incoming connections
    if (listen(listener, SOMAXCONN) == SOCKET_ERROR)
    {
        std::cerr << "listen() failed: " << WSAGetLastError() << "\n";
        closesocket(listener);
        WSACleanup();
        return 1;
    }
    std::cout << "Listening on port 8888...\n";

    // 5. Serve clients one at a time, forever
    while (true)
    {
        SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET)
            continue;

        char buf[4096];
        int n = recv(client, buf, sizeof(buf) - 1, 0);
        if (n > 0)
        {
            std::string req(buf, n);
            std::string firstLine = req.substr(0, req.find("\r\n"));
            std::cout << "REQUEST: " << firstLine << "\n";

            std::string body = "Hello from NetPulse proxy\n";
            std::string reply =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: " +
                std::to_string(body.size()) + "\r\n"
                                              "Connection: close\r\n"
                                              "\r\n" +
                body;
            send(client, reply.c_str(), (int)reply.size(), 0);
        }
        closesocket(client);
    }

    closesocket(listener);
    WSACleanup();
    return 0;
}