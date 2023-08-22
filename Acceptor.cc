#include "Acceptor.h"
#include "Logger.h"
#include "InetAddress.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <functional>
#include <unistd.h>

static int createNonblocking() {
    int sockfd = ::socket( AF_INET , SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC , 0 );
    if (sockfd < 0) {
        LOG_FATAL( "%s:%s:%d listen socket create err:%d \n" , __FILE__ , __FUNCTION__ , __LINE__ , errno );
    }
}

Acceptor::Acceptor( EventLoop *loop , const InetAddress &listenAddr , bool reuseport )
    : loop_( loop )
    , acceptSocket_( createNonblocking() )  // 创建监听socket，并封装成Socket对象（管理socket选项和开关）
    , acceptChannel_( loop , acceptSocket_.fd() ) // 封装成Channel（管理事件及事件回调）
    , listenning_( false ) {
    acceptSocket_.setReuseAddr( true );
    acceptSocket_.setReusePort( true );
    acceptSocket_.bindAddress( listenAddr ); // bind
    // 注册连接socket的读事件的回调函数
    // TcpServer::start() => Acceptor::listen => 有新用户连接，执行回调 => connfd => Channel => subloop
    acceptChannel_.setReadCallback( std::bind( &Acceptor::handleRead , this ) );
}

Acceptor::~Acceptor() {
    /* 注意：监听socket由Socket的析构函数关闭 */
    acceptChannel_.disableAll();
    acceptChannel_.remove();
}

void Acceptor::listen() {
    listenning_ = true;
    acceptSocket_.listen(); // listen
    acceptChannel_.enableReading();
}


// listenfd有读事件发生了，就是有新用户连接了
void Acceptor::handleRead() {
    InetAddress peerAddr;
    int connfd = acceptSocket_.accept( &peerAddr );
    if (connfd >= 0) {
        if (newConnectionCallback_) {
            newConnectionCallback_( connfd , peerAddr ); // 轮询找到subLoop，唤醒，分发当前的新连接的Channel
        }
        else {
            ::close( connfd );
        }
    }
    else {
        LOG_ERROR( "%s:%s:%d accpet err:%d \n" , __FILE__ , __FUNCTION__ , __LINE__ , errno );
        if (errno == EMFILE) {
            LOG_ERROR( "%s:%s:%d sockfd reached limit \n" , __FILE__ , __FUNCTION__ , __LINE__);
        }
    }
}


