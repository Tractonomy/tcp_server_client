
#include "tcp_client.h"
#include "common.h"
#include <netinet/tcp.h>

TcpClient::TcpClient() {
    _isConnected = false;
    _isClosed = true;
}

TcpClient::~TcpClient() {
    close();
}

pipe_ret_t TcpClient::connectTo(
    const std::string & address, int port,
    const std::string & client_addr, int client_port) {
    // ATRS-1429 (defect #2): a caller may invoke connectTo() without first
    // calling close() (e.g. PBHandler::connect() does this on reconnect). If a
    // live socket / receive thread still exists, tear it down here so the new
    // connection does not inherit a stale recv thread blocked in select() on
    // the previous fd.
    if (!_isClosed) {
        close();
    }

    // ATRS-1429 (defect #3): every failure-return path below must close the
    // socket fd allocated by initializeSocket(). Previously the fd was leaked
    // because FileDescriptor::set() does not close-on-overwrite and _isClosed
    // remained `true` (so the guard above is skipped on the next retry, and
    // initializeSocket() simply orphans the previous fd). At 1 Hz retry this
    // exhausts RLIMIT_NOFILE in ~17 minutes, after which socket() returns
    // EMFILE and the driver is wedged ("Too many open files").
    try {
        initializeSocket();
        setAddress(address, port);
        setClientAddress(client_addr, client_port);
    } catch (const std::runtime_error& error) {
        // initializeSocket() throws only when socket() itself failed, in
        // which case _sockfd holds -1 and nothing needs closing. setAddress()
        // / setClientAddress() throw after socket() has succeeded, so the fd
        // must be released here.
        if (_sockfd.get() >= 0) {
            ::close(_sockfd.get());
            _sockfd.set(-1);
        }
        return pipe_ret_t::failure(error.what());
    }

    const int bindResult = bind(_sockfd.get(), (struct sockaddr *)&_client, sizeof(_client));
    if (bindResult == -1) {
        const int savedErrno = errno;
        ::close(_sockfd.get());
        _sockfd.set(-1);
        return pipe_ret_t::failure(strerror(savedErrno));
    }

    const int connectResult = connect(_sockfd.get() , (struct sockaddr *)&_server , sizeof(_server));
    const bool connectionFailed = (connectResult == -1);
    if (connectionFailed) {
        const int savedErrno = errno;
        ::close(_sockfd.get());
        _sockfd.set(-1);
        return pipe_ret_t::failure(strerror(savedErrno));
    }

    // ATRS-1429 (defect #1): order matters. Set _isConnected BEFORE spawning
    // the receive thread. receiveTask()'s outer loop is `while(_isConnected)`;
    // if the new thread runs before this assignment it observes the leftover
    // `false` from the previous terminateReceiveThread() and exits
    // immediately. Sends keep succeeding (queued into the kernel TX buffer)
    // until backpressure produces EPIPE much later, while nothing is
    // consuming the RX buffer — exactly the dead-state where the driver
    // believes it is connected but the kernel Recv-Q grows unbounded and no
    // MCU response is ever delivered to the ROS callback.
    _isConnected = true;
    _isClosed = false;
    startReceivingMessages();

    return pipe_ret_t::success();
}

void TcpClient::startReceivingMessages() {
    _receiveTask = new std::thread(&TcpClient::receiveTask, this);
}

void TcpClient::initializeSocket() {
    pipe_ret_t ret;

    _sockfd.set(socket(AF_INET , SOCK_STREAM , IPPROTO_TCP));
    const bool socketFailed = (_sockfd.get() == -1);
    if (socketFailed) {
        throw std::runtime_error(strerror(errno));
    }

    // // timeout of receive set to 0 as we do not want to disconnect when nothing is received
    struct timeval tv_recv = {
        .tv_sec = 0,
        .tv_usec = 0,
    };
    // set timeout for send to inform user of slow connection
    struct timeval tv_send = {
        .tv_sec = 0,
        .tv_usec = 100000,
    };

    if (setsockopt(_sockfd.get(), SOL_SOCKET, SO_RCVTIMEO, &tv_recv, sizeof(tv_recv)) == -1) {
        std::cerr << "RCVTIMEO error" << std::endl;
    }
    if (setsockopt(_sockfd.get(), SOL_SOCKET, SO_SNDTIMEO, &tv_send, sizeof(tv_send)) == -1) {
        std::cerr << "SNDTIMEO error" << std::endl;
    }

    int option = 1;
    if (setsockopt(_sockfd.get(), SOL_SOCKET, SO_REUSEADDR, &option, sizeof(int)) == -1) {
        std::cerr << "REUSEADDR error" << std::endl;
    }
    if (setsockopt(_sockfd.get(), SOL_SOCKET, SO_REUSEPORT, &option, sizeof(int)) == -1) {
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
    if (setsockopt(_sockfd.get(), SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive)) == -1) {
        std::cerr << "KEEPALIVE error" << std::endl;
    }
    //set the keepalive options
    if (setsockopt(_sockfd.get(), IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt)) != 0) {
        std::cerr << "TCP_KEEPCNT error" << std::endl;
    }
    if (setsockopt(_sockfd.get(), IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle)) != 0) {
        std::cerr << "TCP_KEEPIDLE error" << std::endl;
    }
    if (setsockopt(_sockfd.get(), IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl)) != 0) {
        std::cerr << "TCP_KEEPINTVL error" << std::endl;
    }
}

void TcpClient::setAddress(const std::string& address, int port) {
    const int inetSuccess = inet_aton(address.c_str(), &_server.sin_addr);

    if(!inetSuccess) { // inet_addr failed to parse address
        // if hostname is not in IP strings and dots format, try resolve it
        struct hostent *host;
        struct in_addr **addrList;
        if ( (host = gethostbyname( address.c_str() ) ) == nullptr){
            throw std::runtime_error("Failed to resolve hostname");
        }
        addrList = (struct in_addr **) host->h_addr_list;
        _server.sin_addr = *addrList[0];
    }
    _server.sin_family = AF_INET;
    _server.sin_port = htons(port);
}

void TcpClient::setClientAddress(const std::string& address, int port) {
    // Explicitly assigning port from parameters
    // binding client with that port
    // this allows multiple clients in same process to define different port
    _client.sin_family = AF_INET;
    _client.sin_addr.s_addr = INADDR_ANY;
    _client.sin_port = htons(port);

    // This ip address will change according to the machine
    _client.sin_addr.s_addr = inet_addr(address.c_str());
}


pipe_ret_t TcpClient::sendMsg(const char * msg, size_t size) {
    if (!_isConnected) {
        return pipe_ret_t::failure("not connected, not sending");
    }
    if (_isClosed) {
        return pipe_ret_t::failure("client closed, not sending");
    }

    // MSG_NOSIGNAL: do NOT raise SIGPIPE if the peer has closed the socket;
    // send() returns -1 with errno=EPIPE instead, which we handle below.
    // Without this the default SIGPIPE action terminates the process.
    const ssize_t numBytesSent = send(_sockfd.get(), msg, size, MSG_NOSIGNAL);

    if (numBytesSent < 0) { // send failed
        const int savedErrno = errno;
        // Fatal socket errors (peer closed, connection reset, broken pipe, bad fd,
        // host unreachable, etc.) mean the connection is dead. Notify subscribers so
        // the reconnect logic kicks in; otherwise every subsequent send fails forever
        // because the receive thread may not observe the disconnect on its own (e.g.
        // when the peer RSTs while we are blocked in select).
        if (savedErrno == EPIPE || savedErrno == ECONNRESET ||
            savedErrno == ENOTCONN || savedErrno == EBADF ||
            savedErrno == ECONNABORTED || savedErrno == EHOSTUNREACH ||
            savedErrno == ENETUNREACH || savedErrno == ENETDOWN ||
            savedErrno == ENETRESET || savedErrno == ESHUTDOWN) {
            if (_isConnected.exchange(false)) {
                publishServerDisconnected(pipe_ret_t::failure(strerror(savedErrno)));
            }
        }
        return pipe_ret_t::failure(strerror(savedErrno));
    }
    if (static_cast<size_t>(numBytesSent) < size) { // not all bytes were sent
        char errorMsg[100];
        sprintf(errorMsg, "Only %zd bytes out of %zu was sent to client", numBytesSent, size);
        return pipe_ret_t::failure(errorMsg);
    }
    return pipe_ret_t::success();
}

void TcpClient::subscribe(const client_observer_t & observer) {
    std::lock_guard<std::mutex> lock(_subscribersMtx);
    _subscibers.push_back(observer);
}

void TcpClient::unsubscribeAll()
{
    std::lock_guard<std::mutex> lock(_subscribersMtx);
    _subscibers.clear();
}

/*
 * Publish incomingPacketHandler client message to observer.
 * Observers get only messages that originated
 * from clients with IP address identical to
 * the specific observer requested IP
 */
void TcpClient::publishServerMsg(const char * msg, size_t msgSize) {
    std::lock_guard<std::mutex> lock(_subscribersMtx);
    for (const auto &subscriber : _subscibers) {
        if (subscriber.incomingPacketHandler) {
            subscriber.incomingPacketHandler(msg, msgSize);
        }
    }
}

/*
 * Publish client disconnection to observer.
 * Observers get only notify about clients
 * with IP address identical to the specific
 * observer requested IP
 */
void TcpClient::publishServerDisconnected(const pipe_ret_t & ret) {
    std::lock_guard<std::mutex> lock(_subscribersMtx);
    for (const auto &subscriber : _subscibers) {
        if (subscriber.disconnectionHandler) {
            subscriber.disconnectionHandler(ret);
        }
    }
}

/*
 * Receive server packets, and notify user
 */
void TcpClient::receiveTask() {
    while(_isConnected) {
        const fd_wait::Result waitResult = fd_wait::waitFor(_sockfd);

        if (waitResult == fd_wait::Result::FAILURE) {
            throw std::runtime_error(strerror(errno));
        } else if (waitResult == fd_wait::Result::TIMEOUT) {
            continue;
        }

        char msg[MAX_PACKET_SIZE];
        // recv() returns ssize_t (can be -1 on error). Using size_t here silently
        // turns -1 into SIZE_MAX, which is NOT < 1, so abrupt disconnects (RST,
        // ECONNRESET) used to bypass the disconnect handler entirely.
        const ssize_t numOfBytesReceived = recv(_sockfd.get(), msg, MAX_PACKET_SIZE, 0);

        if(numOfBytesReceived < 1) {
            std::string errorMsg;
            if (numOfBytesReceived == 0) { //server closed connection
                errorMsg = "Server closed connection";
            } else {
                errorMsg = strerror(errno);
            }
            _isConnected = false;
            publishServerDisconnected(pipe_ret_t::failure(errorMsg));
            return;
        } else {
            publishServerMsg(msg, static_cast<size_t>(numOfBytesReceived));
        }
    }
}

void TcpClient::terminateReceiveThread() {
    _isConnected = false;

    if (_receiveTask) {
        _receiveTask->join();
        delete _receiveTask;
        _receiveTask = nullptr;
    }
}

pipe_ret_t TcpClient::close(){
    if (_isClosed) {
        return pipe_ret_t::failure("client is already closed");
    }
    terminateReceiveThread();

    const bool closeFailed = (::close(_sockfd.get()) == -1);
    if (closeFailed) {
        return pipe_ret_t::failure(strerror(errno));
    }
    _isClosed = true;
    return pipe_ret_t::success();
}


