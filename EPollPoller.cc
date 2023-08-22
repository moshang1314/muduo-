#include "EPollPoller.h"
#include "Logger.h"
#include "Channel.h"

#include <errno.h>
#include <unistd.h>
#include <string.h>

// channel 未添加到poller中
const int kNew = -1;    // channel的成员index_被初始化为-1
// channel 已添加到poller中
const int kAdded = 1;
// channel从poller中删除
const int kDeleted = 2;

EPollPoller::EPollPoller( EventLoop* loop )
    : Poller( loop )
    , epollfd_( ::epoll_create1( EPOLL_CLOEXEC ) )
    , events_( 16 ) {
    if (epollfd_ < 0) {
        LOG_FATAL( "epoll_create error:%d \n" , errno );
    }
}

EPollPoller::~EPollPoller() {
    ::close( epollfd_ );
}

Timestamp EPollPoller::poll( int timeoutMs , ChannelList* activeChannels ) {
    LOG_DEBUG( "func=%s => fd total count:%d\n" , channels_.size() );

    int numEvents = ::epoll_wait( epollfd_ , &*events_.begin() , static_cast<int>( events_.size() ) , timeoutMs );
    int savedErrno = errno;
    Timestamp now( Timestamp::now() );

    if (numEvents > 0) {
        LOG_DEBUG( "%d events happened \n" , numEvents );
        fillActiveChannels( numEvents , activeChannels );
        if (numEvents == events_.size()) {
            //扩容以获取更多已发生的事件
            events_.resize( events_.size() * 2 );
        }
    }
    else if (numEvents == 0) {
        LOG_DEBUG( "%s timeout! \n" , __FUNCTION__ );
    }
    else {
        if (savedErrno != EINTR) {
            errno = savedErrno;
            LOG_ERROR( "EPollPoller::poll() err!" );
        }
    }
    return now;
}

/**
 *          EventLoop
 *      ChannelList     Poller
 *                      ChannelMap <fd, channel*>
*/
void EPollPoller::updateChannel( Channel* channel ) {
    const int index = channel->index();
    LOG_INFO( "func=%s => fd=%d events=%d index=%d \n" , __FUNCTION__, channel->fd() , channel->events() , index );

    if (index == kNew || index == kDeleted) {
        if (index == kNew) {
            int fd = channel->fd();
            channels_[fd] = channel;
        }
        // 注意，删除后channel还在map中，只是index=kDeleted
        channel->set_index( kAdded );
        update( EPOLL_CTL_ADD , channel );
    }
    else {  
        int fd = channel->fd();
        if (channel->isNoneEvent()) {
            update( EPOLL_CTL_DEL , channel );
            channel->set_index( kDeleted );
        }
        else {
            update( EPOLL_CTL_MOD , channel );
        }
    }
}

// 从poller中删除channel，包括从map中移除和从epollfd树中移除
void EPollPoller::removeChannel( Channel* channel ) {
    int fd = channel->fd();
    channels_.erase( fd );

    LOG_INFO( "func=%s => fd=%d\n" , __FUNCTION__, fd );

    int index = channel->index();
    if (index == kAdded) {
        update( EPOLL_CTL_DEL , channel );
    }
    channel->set_index( kNew );
}

void EPollPoller::fillActiveChannels( int numEvents , ChannelList* activeChannels ) const {
    for (int i = 0; i < numEvents; ++i) {
        Channel* channel = static_cast<Channel*>( events_[i].data.ptr );
        channel->set_revents( events_[i].events );
        activeChannels->push_back( channel );   // EventLoop就拿到了poller给它返回的所有发生事件的channel列表
    }
}

void EPollPoller::update( int operation , Channel* channel ) {
    epoll_event event;
    memset( &event , 0 , sizeof event );
    event.events = channel->events();
    event.data.ptr = channel;
    int fd = channel->fd();
    
    if (::epoll_ctl( epollfd_ , operation , fd , &event ) < 0) {
        if (operation == EPOLL_CTL_DEL) {
            LOG_ERROR( "epoll_ctl del error:%d\n" , errno );
        }
        else {
            LOG_FATAL( "epoll_ctl add/mod error:%d\n" , errno );
        }
    }
}