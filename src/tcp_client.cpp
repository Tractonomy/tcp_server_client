#include "../include/tcp_client.h"
#include <netinet/tcp.h>


pipe_ret_t TcpClient::connectTo(
  const std::string & server_addr,
  int server_port,
  const std::string & client_addr,
  int client_port)
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

  // timeout of receive set to 0 as we do not want to disconnect when nothing is received
  struct timeval tv_recv = {
    .tv_sec = 0,
    .tv_usec = 0,
  };
  // set timeout for send to inform user of slow connection
  struct timeval tv_send = {
    .tv_sec = 0,
    .tv_usec = 100000,
  };

  if (setsockopt(m_sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv_recv, sizeof(tv_recv)) == -1) {
    std::cerr << "RCVTIMEO error" << std::endl;
  }
  if (setsockopt(m_sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv_send, sizeof(tv_send)) == -1) {
    std::cerr << "SNDTIMEO error" << std::endl;
  }

  int option = 1;
  if (setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(int)) == -1) {
    std::cerr << "REUSEADDR error" << std::endl;
  }
  if (setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEPORT, &option, sizeof(int)) == -1) {
    std::cerr << "REUSEPORT error" << std::endl;
  }

  /** Enable keep alive mode
   */
  int keepalive = 1;
  /** The time (in seconds) the connection needs to remain
   * idle before TCP starts sending keepalive probes (TCP_KEEPIDLE socket option)
   */
  int keepidle = 1;
  /** The maximum number of keepalive probes TCP should
   * send before dropping the connection. (TCP_KEEPCNT socket option)
   */
  int keepcnt = 3;
  /** The time (in seconds) between individual keepalive probes.
   *  (TCP_KEEPINTVL socket option)
   */
  int keepintvl = 1;
  if (setsockopt(m_sockfd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive)) == -1) {
    std::cerr << "KEEPALIVE error" << std::endl;
  }
  //set the keepalive options
  if (setsockopt(m_sockfd, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt)) != 0) {
    std::cerr << "TCP_KEEPCNT error" << std::endl;
  }
  if (setsockopt(m_sockfd, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle)) != 0) {
    std::cerr << "TCP_KEEPIDLE error" << std::endl;
  }
  if (setsockopt(m_sockfd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl)) != 0) {
    std::cerr << "TCP_KEEPINTVL error" << std::endl;
  }

  int inetSuccess = inet_aton(server_addr.c_str(), &m_server.sin_addr);

  if (!inetSuccess) {  // inet_addr failed to parse address
    // if hostname is not in IP strings and dots format, try resolve it
    struct hostent * host;
    struct in_addr ** addrList;
    if ( (host = gethostbyname(server_addr.c_str() ) ) == NULL) {
      ret.success = false;
      ret.msg = "Failed to resolve hostname";
      return ret;
    }
    addrList = (struct in_addr **) host->h_addr_list;
    m_server.sin_addr = *addrList[0];
  }
  m_server.sin_family = AF_INET;
  m_server.sin_port = htons(server_port);

  struct sockaddr_in m_client;
  // Explicitly assigning port from paramters
  // binding client with that port
  // this allows multiple clients in same process to define different port
  m_client.sin_family = AF_INET;
  m_client.sin_addr.s_addr = INADDR_ANY;
  m_client.sin_port = htons(client_port);

  // This ip address will change according to the machine
  m_client.sin_addr.s_addr = inet_addr(client_addr.c_str());

  int bindRet = bind(m_sockfd, (struct sockaddr *)&m_client, sizeof(m_client));
  if (bindRet == -1) {
    ret.success = false;
    ret.msg = strerror(errno);
    return ret;
  }

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
    ret.code = errno;
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
    if (numOfBytesReceived < 0) {
      pipe_ret_t ret;
      ret.success = false;
      ret.msg = strerror(errno);
      std::cerr << ret.msg << std::endl;
      publishServerDisconnected(ret);
      finish();
      break;
    } else if (numOfBytesReceived > 0) {
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
  printf("shutting down\r\n");
  shutdown(m_sockfd, SHUT_RDWR);
  finish();
}
