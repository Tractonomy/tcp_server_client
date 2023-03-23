//
// Created by erauper on 4/7/19.
//

#ifndef INTERCOM_TCP_CLIENT_H
#define INTERCOM_TCP_CLIENT_H


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
#include <vector>
#include <errno.h>
#include <thread>
#include "client_observer.h"
#include "pipe_ret_t.h"

#define MAX_PACKET_SIZE 2048


//TODO: REMOVE ABOVE CODE, AND SHARE client.h FILE WITH SERVER AND CLIENT

class TcpClient
{
private:
  int m_sockfd = 0;
  bool stop = false;
  bool connected = false;
  struct sockaddr_in m_server;
  std::vector<client_observer_t> m_subscibers;
  std::thread * m_receiveTask = nullptr;

  void publishServerMsg(const char * msg, size_t msgSize);
  void publishServerDisconnected(const pipe_ret_t & ret);
  void ReceiveTask();
  void terminateReceiveThread();

public:
  ~TcpClient();
  pipe_ret_t connectTo(
    const std::string & server_addr,
    int server_port,
    const std::string & client_addr = "0.0.0.0",
    int client_port = 0);
  pipe_ret_t sendMsg(const char * msg, size_t size);

  void subscribe(const client_observer_t & observer);
  void unsubscribeAll();

  pipe_ret_t finish();
};

#endif //INTERCOM_TCP_CLIENT_H
