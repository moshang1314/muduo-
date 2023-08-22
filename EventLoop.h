#pragma once

#include <vector>
#include <atomic>
#include <memory>
#include <mutex>
#include <functional>

#include "noncopyable.h"
#include "Timestamp.h"
#include "CurrentThread.h"

class Channel;
class Poller;

class EventLoop : noncopyable{
public:
    using Functor = std::function<void()>;

    EventLoop();
    ~EventLoop();

    // 开启事件循环
    void loop();
    // 退出事件循环
    void quit();

    Timestamp pollReturnTime() const { return pollReturnTime_; }

    // 在当前loop中执行cb
    void runInLoop( Functor cb );
    // 把cb放入队列中，唤醒loop
    void queueInLoop( Functor cb );

    // 用来唤醒loop所在线程的
    void wakeup();
    
    void updateChannel( Channel *channel );
    void removeChannel( Channel *channel );
    bool hasChannel( Channel *channel );

    // 判断EventLoop对象是否在当前线程里
    bool isInLoopThread() const { return threadId_ == CurrentThread::tid(); }
private:
    // wake up
    void handleRead();
    // 执行回调
    void doPendingFunctors();
    
    using ChannelList = std::vector<Channel *>;

    std::atomic_bool looping_;  //原子操作，通过CAS实现
    std::atomic_bool quit_;     //标识退出loop循环
    
    const pid_t threadId_;  // 记录当前loop所在的线程Id
    Timestamp pollReturnTime_;  //poller返回发生事件的channels的时间点
    std::unique_ptr<Poller> poller_;

    int wakeupFd_;  //当mainLoop获取一个新用户的channel，通过轮询算法选择一个subloop，通过该成员唤醒subloop处理
    std::unique_ptr<Channel> wakeupChannel_;

    ChannelList activeChannels_;

    std::atomic_bool callingPendingFunctors_;   // 标识当前loop是否有需要执行的回调操作
    std::vector<Functor> pendingFunctors_;  // 存储loop需要执行的所有的回调操作
    std::mutex mutex_;
};