#include "EventLoop.h"
#include "Logger.h"
#include "Poller.h"
#include "Channel.h"

#include <sys/eventfd.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <memory>

// 防止一个线程创建多个EventLoop thread_local
__thread EventLoop *t_loopInThisThread = nullptr;

// 定义默认的Poller IO复用接口的超时时间
const int kPollTimeMs = 10000;

// 创建wakeupfd，用来notify唤醒subReactor处理新来的channel
int createEventfd() {
    int evtfd = ::eventfd( 0 , EFD_NONBLOCK | EFD_CLOEXEC );
    if (evtfd < 0) {
        LOG_FATAL( "eventfd error:%d \n" , errno );
    }
    return evtfd;
}

EventLoop::EventLoop()
    :looping_( false )
    , quit_( false )
    , callingPendingFunctors_( false )  /* 表征loop是否在执行用户注册的回调中 */
    , threadId_( CurrentThread::tid() ) /* 当前线程Id，loop只会在创建其的线程上运行 */
    , poller_( Poller::newDefaultPoller( this ) ) /* 根据系统环境变量创建Poller对象，可为epoll和poll */
    , wakeupFd_( createEventfd() )  /* 创建eventfd，用于其它线程唤醒当前线程执行及时执行注册在当前loop上的回调 */
    , wakeupChannel_( new Channel( this , wakeupFd_ ) )/* 将eventfd封装到Channel */{
    LOG_DEBUG( "EventLoop created %p in thread %d \n" , this , threadId_ );
    if (t_loopInThisThread) {
        LOG_FATAL( "Another EventLoop %p exists in this thread %d \n" , t_loopInThisThread , threadId_ );
    }
    else {
        t_loopInThisThread = this;
    }

    // 设置eventfd感兴趣的事件类型和发生的事件后的回调操作
    wakeupChannel_->setReadCallback( std::bind( &EventLoop::handleRead , this ) );
    // 设置读事件，并将eventfd设置到对应loop的poller上
    wakeupChannel_->enableReading();
}  

EventLoop::~EventLoop() {
    wakeupChannel_->disableAll();
    // 将eventfd对应的channel从loop中移除
    // channel被智能指针管理不需要显式释放
    wakeupChannel_->remove();
    ::close( wakeupFd_ );
    t_loopInThisThread = nullptr;
}

// 开启事件循环
void EventLoop::loop() {
    looping_ = true;
    quit_ = false;

    LOG_INFO( "EventLoop %p start looping \n" , this );

    while (!quit_) {
        activeChannels_.clear();
         pollReturnTime_ = poller_->poll( kPollTimeMs , &activeChannels_ );
        for (Channel *channel : activeChannels_) {
            // Poller监听哪些channel发生事件了，然后上报给EventLoop，通知channel处理相应的的事件
            channel->handleEvent( pollReturnTime_ );
        }
        // 执行当前EventLoop时间内循环需要处理的回调操作
        /**
        * mainLoop 事先注册一个回调cb（需要subloop来执行） wakeup subloop后，执行下面的方法，执行之前mainloop注册的cb操作
        */
        doPendingFunctors();
    }

    LOG_INFO( "EventLoop %p stop looping. \n" , this );
    looping_ = false;
}

/** 退出事件循环
 * 1. loop在自己的线程中调用quit
 * 2. 在非loop的线程中，调用loop的quit，此时需要先唤醒该subloop
*/
void EventLoop::quit() {
    quit_ = true;

    if (!isInLoopThread()) {
        wakeup();
    }
}

 // 在当前loop中执行cb
void EventLoop::runInLoop( Functor cb ) {
    if (isInLoopThread()) {
        cb();
    }
    else {  //在非当前loop线程中执行，就需要先唤醒loop所在线程，执行cb
        queueInLoop( cb );
    }
}

// 把cb放入队列中，唤醒loop
void EventLoop::queueInLoop( Functor cb ) {
    {
        std::unique_lock<std::mutex> lock( mutex_ );
        pendingFunctors_.emplace_back( cb );
    }
    // 唤醒相应的，需要执行回调操作的loop的线程了
    // callingPendingFunctors_解释：当回调正在执行过程中，执行完后马上会阻塞在epoll_wait处，因此，也需要通过写wakeupfd来唤醒它，来执行新注册的回调函数
    if (!isInLoopThread() || callingPendingFunctors_) {
        wakeup();   // 唤醒loop所在线程
    }
}

void EventLoop::handleRead() {
    uint64_t one = 1;
    ssize_t n = read( wakeupFd_ , &one , sizeof one );
    if (n != sizeof one) {
        LOG_ERROR( "EventLoop::handleRead() reads %ld bytes instead of 8" , n );
    }
}

 // 用来唤醒loop所在线程的, 向wakeupfd_写一个数据，wakeupChannel上就会发生读事件，当前loop线程就会被唤醒
void EventLoop::wakeup() {
    uint64_t one = 1;
    ssize_t n = write( wakeupFd_ , &one , sizeof one );
    if (n != sizeof one) {
        LOG_ERROR( "EventLoop::wakeup() writes %ld bytes instead of 8 \n" , n );
    }
}
    
void EventLoop::updateChannel( Channel *channel ) {
    poller_->updateChannel( channel );
}

void EventLoop::removeChannel( Channel *channel ) {
    poller_->removeChannel( channel );
}

bool EventLoop::hasChannel( Channel *channel ) {
    return poller_->hasChannel( channel );
}

// 执行回调
void EventLoop::doPendingFunctors() {
    std::vector< Functor> functors;
    callingPendingFunctors_ = true;

    {
        std::unique_lock<std::mutex> lock( mutex_ );
        functors.swap( pendingFunctors_ );
    }

    for (const Functor &functor : functors) {
        functor();
    }
    callingPendingFunctors_ = false;
}