#include "TcpServer.h"
#include "Logger.h"
#include "TcpConnection.h"

#include <functional>
#include <string>
#include <strings.h>

static EventLoop *CheckLoopNotNull( EventLoop *loop ) {
    if (loop == nullptr) {
        LOG_FATAL( "%s:%s:%d mainLoop is null! \n" , __FILE__ , __FUNCTION__ , __LINE__ );
    }
}

TcpServer::TcpServer( EventLoop *loop , /* mainLoop, running in mainThread */
    const InetAddress &listenAddr ,
    const std::string &nameArg,
    Option option )
    : loop_( CheckLoopNotNull( loop ) )
    , ipPort_( listenAddr.toIpPort() )
    , name_( nameArg )
    , acceptor_( new Acceptor( loop , listenAddr , option == kReusePort ) ) /* 创建监听socket，并封装到acceptor和其内部的Channel和Socket，由mainLoop管理 */
    , threadPool_( new EventLoopThreadPool( loop , name_ ) ) /* 创建EventLoopThreadPool对象，以管理EventLoop对象和线程 */
    , connectionCallback_()
    , messageCallback_()
    , nextConnId_( 1 ) {
    // 当有新用户连接时，listenfd发生的读事件，在其读事件处理过程中会调用该回调将新连接分配到子线程去处理
    acceptor_->setNewConnectionCallback( std::bind( &TcpServer::newConnection , this , std::placeholders::_1 , std::placeholders::_2 ) );
}

TcpServer::~TcpServer() {
    for (auto &item : connections_) {
        TcpConnectionPtr conn( item.second );
        item.second.reset();

        conn->getLoop()->runInLoop(
            std::bind( &TcpConnection::connectDestroyed , conn )
        );
    }
}

// 设置底层subloop的个数
void TcpServer::setThreadNum( int numThreads ) {
    threadPool_->setThreadNum( numThreads );
}

// 开启服务器监听
void TcpServer::start() {
    if (started_++ == 0) {
        threadPool_->start( threadInitCallback_ );  // 启动底层的loop线程池
        /* 设置监听socket为被动监听，并设置其读事件上的回调，将其交给mainLoop处理 */
        loop_->runInLoop( std::bind( &Acceptor::listen , acceptor_.get() ) );
    }
}

/* 它将在监听socket的读事件回调中被调用 */
void TcpServer::newConnection( int sockfd , const InetAddress &peerAddr ) {
    // 轮询算法，选择一个subLoop，来管理channel
    EventLoop *ioLoop = threadPool_->getNextLoop();
    char buf[64] = { 0 };
    snprintf( buf , sizeof buf , "-%s#%d" , ipPort_.c_str() , nextConnId_ );
    ++nextConnId_;
    std::string connName = name_ + buf;

    LOG_INFO( "TcpServer::newConnection [%s] - new connection [%s] from %s \n" , name_.c_str() ,
        connName.c_str() , peerAddr.toIpPort().c_str() );

    sockaddr_in local;
    ::bzero( &local , sizeof local );
    socklen_t addrlen = sizeof local;
    if (::getsockname( sockfd , (sockaddr *)&local , &addrlen ) < 0) {
        LOG_ERROR( "sockets::getLocalAddr" );
    }
    InetAddress localAddr( local );

    // 根据连接成功的sockfd，创建TcpConnection连接对象
    TcpConnectionPtr conn( new TcpConnection(
        ioLoop ,
        connName ,
        sockfd ,
        localAddr ,
        peerAddr
    ) );
    connections_[connName] = conn;
    // 下面的回调都是用户设置给TcpServer => TcpConnection => Channel => Poller channel 调用回调
    conn->setConnectionCallback( connectionCallback_ );
    conn->setMessageCallback( messageCallback_ );
    conn->setWriteCompleteCallback( writeCompleteCallback_ );

    // 设置如何关闭连接的回调
    conn->setCloseCallback( std::bind( &TcpServer::removeConnection , this , std::placeholders::_1 ) );
    // 调用连接建立的回调函数，设置conn->state_ = Connected，设置新连接的读事件(将connsocket写入epollfd监听)，并调用用户设置的连接回调函数
    ioLoop->runInLoop( std::bind( &TcpConnection::connectEstablished , conn ) );
}

void TcpServer::removeConnection( const TcpConnectionPtr &conn ) {
    loop_->runInLoop(
        std::bind( &TcpServer::removeConnectionInLoop , this , conn )
    );
}

void TcpServer::removeConnectionInLoop( const TcpConnectionPtr &conn ) {
    LOG_INFO( "TcpServer::removeConnectionInLoop [%s] - connection %s\n" , name_.c_str() , conn->name().c_str() );
    size_t n = connections_.erase( conn->name() );
    EventLoop *ioLoop = conn->getLoop();
    ioLoop->queueInLoop( std::bind( &TcpConnection::connectDestroyed , conn ) );
}