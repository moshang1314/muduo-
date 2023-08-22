#include "TcpConnection.h"
#include "Logger.h"
#include "Socket.h"
#include "Channel.h"
#include "EventLoop.h"

#include <functional>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <strings.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <string>

static EventLoop *CheckLoopNotNull( EventLoop *loop ) {
    if (loop == nullptr) {
        LOG_FATAL( "%s:%s:%d TcpConnection Loop is null! \n" , __FILE__ , __FUNCTION__ , __LINE__ );
    }
}

TcpConnection::TcpConnection( EventLoop *loop ,
        const std::string &nameArg ,
        int sockfd ,
        const InetAddress &localAddr ,
    const InetAddress &peerAddr )
    : loop_( CheckLoopNotNull( loop ) ) /* 处理该连接的loop，由轮询算法获得 */
    , name_( nameArg )
    , state_( kConnecting ) /* 处于连接构造中 */
    , reading_( true )
    , socket_( new Socket( sockfd ) )   /* 建立一个Socket对象来管理该连接socket的选项和生命周期，socket文件描述符在Socket析构中被关闭 */
    , channel_( new Channel( loop , sockfd ) )  /* 建立一个Channel对象来管理该连接socket关心的读写事件 */
    , localAddr_( localAddr )
    , peerAddr_( peerAddr )
    , highWaterMark_( 64 * 1024 * 1024 ) /* 64M */ {
    /* 设置channel的事件处理函数 */
    channel_->setReadCallback( std::bind( &TcpConnection::handleRead , this , std::placeholders::_1 ) );
    channel_->setWriteCallback( std::bind( &TcpConnection::handleWrite , this ) );
    channel_->setCloseCallback( std::bind( &TcpConnection::handleClose , this ) );
    channel_->setErrorCallback( std::bind( &TcpConnection::handleError , this ) );

    LOG_INFO( "TcpConnection::ctor[%s] at fd=%d\n" , name_.c_str() , sockfd );
    socket_->setKeepAlive( true );
}

TcpConnection::~TcpConnection() {
    LOG_INFO( "TcpConnection::dtor[%s] at fd=%d state=%d \n" , name_.c_str() , channel_->fd() , (int)state_);
}

void TcpConnection::send( const std::string &buf ) {
    if (state_ == kConnected) {
        if (loop_->isInLoopThread()) {
            sendInLoop( buf.c_str() , buf.size() );
        }
        else {
            loop_->runInLoop( std::bind( &TcpConnection::sendInLoop , this , buf.c_str() , buf.size() ) );
        }
    }
}

void TcpConnection::sendInLoop(const void* data, size_t len) {
    ssize_t nwrote = 0;
    size_t remaining = len;
    bool faultError = false;

    // 之前调用过该connection的shutdown，不能再进行发送了
    if (state_ == kDisconnected) {
        LOG_ERROR( "disconnected, give up writing!" );
        return;
    }

    // 没有写缓冲区中待写的数据，且没有设置感兴趣的写事件
    if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0) {
        nwrote = ::write( channel_->fd() , data , len );
        if (nwrote >= 0) {
            remaining = len - nwrote;
            if (remaining == 0 && writeCompleteCallback_) {
                // 既然在这里数据全部发送完成，就不用再给channel设置epollout事件了
                loop_->queueInLoop( std::bind( writeCompleteCallback_ , shared_from_this() ) );
            }
        }
        else { /* nwrote < 0 */
            nwrote = 0;
            if (errno != EWOULDBLOCK) {
                LOG_ERROR( "TcpConnection::sendInLoop" ); 
                if (errno == EPIPE || errno == ECONNRESET) {
                    faultError = true;
                }
            }
        }
    }
    /* 说明当前写，并没有把数据全部发送完，剩余的数据需要保存到写缓冲区当中，然后给channel
    注册epollout事件，poller发现tcp发送缓冲区有多余空间，会通知相应的sock-channel，调用注册的writeCallback_回调方法
    最终调用TcpConnection::handleWrite方法，把发送缓冲区的数据全部发送完 */
    if (!faultError && remaining > 0) {
        // 目前发送缓冲区剩余待发送数据长度
        size_t oldLen = outputBuffer_.readableBytes();
        if (oldLen + remaining >= highWaterMark_ && oldLen < highWaterMark_ && highWaterMarkCallback_) {
            loop_->queueInLoop(std::bind( highWaterMarkCallback_ , shared_from_this() , oldLen + remaining ));
        }
        outputBuffer_.append( (char *)data + nwrote , remaining );
        if (!channel_->isWriting()) {
            channel_->enableWriting(); //注册channel的写事件
        }
    }
}

void TcpConnection::handleRead( Timestamp receiveTime ) {
    int savedErrno = 0;
    ssize_t n = inputBuffer_.readFd( channel_->fd() , &savedErrno );
    if (n > 0) {
        // 已建立连接的用户，有可读事件发生了，调用客户传入的回调函数
        messageCallback_(shared_from_this() , &inputBuffer_ , receiveTime );
    }
    else if (n == 0) {  /* 客户端关闭连接 */
        handleClose();
    }
    else {
        errno = savedErrno;
        LOG_ERROR( "TcpConnection::handleRead" );
        handleError();
    }
}

void TcpConnection::handleWrite() {
    if (channel_->isWriting()) {
        int savedErrno = 0;
        ssize_t n = outputBuffer_.writeFd( channel_->fd() , &savedErrno );
        if (n > 0) {
            outputBuffer_.retrieve( n );
            if (outputBuffer_.readableBytes() == 0/* 数据发送完毕 */) {
                channel_->disableWriting();
                if (writeCompleteCallback_) {
                    // 唤醒loop_对应的thread线程，执行回调，放入队列中，等处理完其它socket的读写事件，再处理回调，优先级低一些
                    loop_->queueInLoop( std::bind( writeCompleteCallback_ , shared_from_this() ) );
                }
                if (state_ == kDisconnecting) {
                    shutdownInLoop();
                }
            }
        }
        else {
            LOG_ERROR( "TCPConnection::handleWrite" );
        }
    }
    else {
        LOG_ERROR( "TcpConnection fd=%d is down, no more writing \n" , channel_->fd() );
    }
}

/* 关闭连接 */
void TcpConnection::handleClose() {
    LOG_INFO( "fd=%d state=%d \n" , channel_->fd() , (int)state_ );
    setState( kDisconnected );
    channel_->disableAll();

    TcpConnectionPtr connPtr( shared_from_this() );
    /* 调用用户设置的连接事件的处理回调 */
    connectionCallback_( connPtr );
    /* 调用TcpServer中设置的关闭连接的回到 */
    closeCallback_( connPtr );
}

void TcpConnection::handleError() {
    int optval; 
    socklen_t optlen = sizeof optval;
    int err = 0;
    if (::getsockopt( channel_->fd() , SOL_SOCKET , SO_ERROR , &optval , &optlen ) < 0) {
        err = errno;
    } 
    else {
        err = optval;
    }
    LOG_ERROR( "TcpConnection::handleError name:%s - SO_ERROR:%d \n" , name_.c_str() , err );
}

// 建立连接
void TcpConnection::connectEstablished() {
    setState( kConnected );
    channel_->tie( shared_from_this() );
    // 设置该连接的socket上的读事件
    channel_->enableReading(); //  向poller注册channel的epollin事件

    // 新连接建立，执行回调
    connectionCallback_( shared_from_this() );
}

// 销毁连接
void TcpConnection::connectDestroyed() {
    if (state_ == kConnected) {
        setState( kDisconnected );
        channel_->disableAll();
        /* 调用用户定义的连接事件的回调函数 */
        connectionCallback_( shared_from_this() );
    }
    channel_->remove(); // 把channel从poller中删除
}

/** 当服务器主动关闭写端时，SEND_SHUTDOWN被设置，
 * 下次将触发EPOLLHUP事件，然后调用handleclose()函数处理资源释放等工作
 **/
void TcpConnection::shutdown() { // 优雅地关闭连接
    if (state_ == kConnected) {
        setState( kDisconnecting );
        loop_->runInLoop( std::bind( &TcpConnection::shutdownInLoop , this ) );
    }
}

void TcpConnection::shutdownInLoop() {
    if (!channel_->isWriting()) {
        socket_->shutdownWrite();
    }
}

