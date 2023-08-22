#pragma once

#include "noncopyable.h"
#include "InetAddress.h"
#include "Callbacks.h"
#include "Buffer.h"
#include "Timestamp.h"

#include <memory>
#include <string>
#include <atomic>

class Channel;
class EventLoop;
class Socket;

class TcpConnection :noncopyable, public std::enable_shared_from_this<TcpConnection>{
public:
    TcpConnection( EventLoop *loop ,
        const std::string &nameArg ,
        int sockfd ,
        const InetAddress &localAddr ,
        const InetAddress &peerAddr );
    ~TcpConnection();

    EventLoop *getLoop()const { return loop_; }
    const std::string &name() const { return name_; }
    const  InetAddress &localAddress() const { return localAddr_; }
    const InetAddress &peerAddress()const { return peerAddr_; }

    bool connected()const { return state_ == kConnected; }

    void send( const std::string &buffer );
    void shutdown();

    void setConnectionCallback( const ConnectionCallback &cb ) {
        connectionCallback_ = cb;
    }
    void setCloseCallback( const CloseCallback &cb ) {
        closeCallback_ = cb;
    }
    void setMessageCallback( const MessageCallback &cb ) {
        messageCallback_ = cb;
    }
    void setWriteCompleteCallback( const WriteCompleteCallback &cb ) {
        writeCompleteCallback_ = cb;
    }
    void setHighWaterMarkCallback( const HighWaterMarkCallback &cb , size_t highWaterMark ) {
        highWaterMarkCallback_ = cb;
        highWaterMark_ = highWaterMark;
    }

    void connectEstablished();
    void connectDestroyed();

    void shutdown();
private:
    enum StateE { kDisconnected , kConnecting , kConnected , kDisconnecting };
    void setState( StateE state ) { state_ = state; }
    
    void handleRead( Timestamp receiveTime );
    void handleWrite();
    void handleClose();
    void handleError();

    void sendInLoop( const void *message , size_t len );
    void shutdownInLoop();

    EventLoop *loop_;   // 这里在多线程下绝对不是baseLoop，因为TcpConnection都是在subLoop里面管理的
    const std::string name_;
    std::atomic_int state_;
    bool reading_;

    std::unique_ptr<Socket> socket_;
    std::unique_ptr<Channel> channel_;

    const InetAddress localAddr_;
    const InetAddress peerAddr_;

    ConnectionCallback connectionCallback_; // 有新连接时的回调
    MessageCallback messageCallback_;   // 有读写消息时的回调
    WriteCompleteCallback writeCompleteCallback_; // 消息发送完成以后的回调
    HighWaterMarkCallback highWaterMarkCallback_;
    CloseCallback closeCallback_;

    size_t highWaterMark_;

    Buffer inputBuffer_;    // 接收数据缓冲区
    Buffer outputBuffer_;    // 发送数据缓冲区
};