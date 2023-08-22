#include "Buffer.h"

#include <errno.h>
#include <sys/uio.h>
#include <unistd.h>

/**
 * 从fd上读取数据，Poller工作在LT模式
*/
ssize_t Buffer::readFd( int fd , int *saveErrno ) {
    char extrabuf[65536];   //栈上的内存空间
    struct iovec vec[2];
    const size_t writable = writableBytes();    // 这是Buffer底层缓冲区剩余的可写空间大小
    vec[0].iov_base = begin() + writerIndex_;
    vec[0].iov_len = writable;

    vec[1].iov_base = extrabuf;
    vec[1].iov_len = sizeof extrabuf;
    const int iovcnt = ( writable < sizeof extrabuf ) ? 2 : 1;  // 当Buffer可写缓冲区空间大于65536B，则只写一个空间
    const ssize_t n = ::readv( fd , vec , iovcnt );
    if (n < 0) {
        *saveErrno = errno;
    }
    else if (n <= writable) {
        writerIndex_ += n;
    }
    else {  // Buffer缓冲区被写满了
        writerIndex_ = buffer_.size();
        append( extrabuf , n - writable );  // writeIndex_开始写
    }
    return n;
}

ssize_t Buffer::writeFd( int fd , int *saveErrno ) {
    ssize_t n = ::write( fd , peek() , readableBytes() );
    if (n < 0) {
        *saveErrno = errno;
    }
    return n;
}