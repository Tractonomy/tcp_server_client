#include "../include/tcp_client.h"


pipe_ret_t TcpClient::connectTo(const std::string & address, int port)
{
  stop = false;
  m_sockfd = 0;
  pipe_ret_t ret;

  m_sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

  if (m_sockfd == -1) {   //socket failed
    ret.success = false;
    ret.msg = strerror(errno);
    return ret;
  }

  // without timeout no automatic reconnect
  // we expect data much higher than 1Hz
  // reconnect after 30sec to make sure MCU has discovered disconnect
  struct timeval tv = {
    .tv_sec = 30
  };
  if (setsockopt(m_sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == -1) {
    std::cerr << "RCVTIMEO error" << std::endl;
  }
  if (setsockopt(m_sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == -1) {
    std::cerr << "SNDTIMEO error" << std::endl;
  }

  int inetSuccess = inet_aton(address.c_str(), &m_server.sin_addr);

  if (!inetSuccess) {  // inet_addr failed to parse address
    // if hostname is not in IP strings and dots format, try resolve it
    struct hostent * host;
    struct in_addr ** addrList;
    if ( (host = gethostbyname(address.c_str() ) ) == NULL) {
      ret.success = false;
      ret.msg = "Failed to resolve hostname";
      return ret;
    }
    addrList = (struct in_addr **) host->h_addr_list;
    m_server.sin_addr = *addrList[0];
  }
  m_server.sin_family = AF_INET;
  m_server.sin_port = htons(port);

  int connectRet = connect(m_sockfd, (struct sockaddr *)&m_server, sizeof(m_server));
  if (connectRet == -1) {
    ret.success = false;
    ret.msg = strerror(errno);
    return ret;
  }

  m_receiveTask = new std::thread(&TcpClient::ReceiveTask, this);
  ret.success = true;
  connected = true;
  return ret;
}


pipe_ret_t TcpClient::sendMsg(const char * msg, size_t size)
{
  pipe_ret_t ret;
  if (!connected) {
    ret.success = false;
    ret.msg = "not connected";
    return ret;
  }
  ssize_t numBytesSent = send(m_sockfd, msg, size, 0);
  if (numBytesSent < 0) {    // send failed
    ret.success = false;
    ret.msg = strerror(errno);
    return ret;
  }
  if ((uint)numBytesSent < size) {   // not all bytes were sent
    ret.success = false;
    char msg[100];
    sprintf(msg, "Only %ld bytes out of %lu was sent to client", numBytesSent, size);
    ret.msg = msg;
    return ret;
  }
  ret.success = true;
  return ret;
}

void TcpClient::subscribe(const client_observer_t & observer)
{
  m_subscibers.push_back(observer);
}

void TcpClient::unsubscribeAll()
{
  m_subscibers.clear();
}

/*
 * Publish incoming client message to observer.
 * Observers get only messages that originated
 * from clients with IP address identical to
 * the specific observer requested IP
 */
void TcpClient::publishServerMsg(const char * msg, size_t msgSize)
{
  for (uint i = 0; i < m_subscibers.size(); i++) {
    if (m_subscibers[i].incoming_packet_func != NULL) {
      (*m_subscibers[i].incoming_packet_func)(msg, msgSize);
    }
  }
}

/*
 * Publish client disconnection to observer.
 * Observers get only notify about clients
 * with IP address identical to the specific
 * observer requested IP
 */
void TcpClient::publishServerDisconnected(const pipe_ret_t & ret)
{
  connected = false;
  for (uint i = 0; i < m_subscibers.size(); i++) {
    if (m_subscibers[i].disconnected_func != NULL) {
      (*m_subscibers[i].disconnected_func)(ret);
    }
  }
}

/*
 * Receive server packets, and notify user
 */
void TcpClient::ReceiveTask()
{

  while (!stop) {
    char msg[MAX_PACKET_SIZE];
    int numOfBytesReceived = recv(m_sockfd, msg, MAX_PACKET_SIZE, 0);
    if (numOfBytesReceived < 1) {
      pipe_ret_t ret;
      ret.success = false;
      if (numOfBytesReceived == 0) {       //server closed connection
        ret.msg = "Server closed connection.";
      } else {
        ret.msg = strerror(errno);
      }
      std::cout << ret.msg << std::endl;
      publishServerDisconnected(ret);
      finish();
      break;
    } else {
      publishServerMsg(msg, numOfBytesReceived);
    }
  }
}

pipe_ret_t TcpClient::finish()
{
  stop = true;
  terminateReceiveThread();
  pipe_ret_t ret;
  if (close(m_sockfd) == -1) {   // close failed
    ret.success = false;
    ret.msg = strerror(errno);
    return ret;
  }
  ret.success = true;
  return ret;
}

void TcpClient::terminateReceiveThread()
{
  if (m_receiveTask != nullptr) {
    m_receiveTask->detach();
    delete m_receiveTask;
    m_receiveTask = nullptr;
  }
}

TcpClient::~TcpClient()
{
  terminateReceiveThread();
}
