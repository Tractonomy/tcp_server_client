#pragma once

#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netdb.h>
#include <vector>
#include <errno.h>
#include <thread>
#include <mutex>
#include <atomic>
#include "client_observer.h"
#include "pipe_ret_t.h"
#include "file_descriptor.h"


class TcpClient
{
private:
    FileDescriptor _sockfd;
    std::atomic<bool> _isConnected;
    std::atomic<bool> _isClosed;
    struct sockaddr_in _server;
    struct sockaddr_in _client;
    std::vector<client_observer_t> _subscibers;
    std::thread * _receiveTask = nullptr;
    std::mutex _subscribersMtx;

    void initializeSocket();
    void startReceivingMessages();
    void setAddress(const std::string& address, int port);
    void setClientAddress(const std::string& address, int port);
    void publishServerMsg(const char * msg, size_t msgSize);
    void publishServerDisconnected(const pipe_ret_t & ret);
    void receiveTask();
    void terminateReceiveThread();

public:
    TcpClient();
    ~TcpClient();
    pipe_ret_t connectTo(
        const std::string & address, int port,
        const std::string & client_addr = "0.0.0.0", int client_port = 0);
    pipe_ret_t sendMsg(const char * msg, size_t size);

    void subscribe(const client_observer_t & observer);
    void unsubscribeAll();
    bool isConnected() const { return _isConnected; }
    pipe_ret_t close();
};

