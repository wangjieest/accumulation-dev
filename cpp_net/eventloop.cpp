#include <iostream>
#include <assert.h>
#include <thread>

#include "channel.h"
#include "eventloop.h"

class WakeupChannel : public Channel
{
public:
    WakeupChannel(int fd)
    {
        mFd = fd;

    }
private:
    void    canRecv() override
    {
#ifdef PLATFORM_WINDOWS
#else
        /*  linux 下必须读完所有数据 */
        char temp[1024 * 10];
        while (true)
        {
            ssize_t n = recv(mFd, temp, 1024 * 10, 0);
            if (n == -1)
            {
                break;
            }
        }
#endif
    }

    void    canSend() override
    {
    }

    void    setEventLoop(EventLoop*) override
    {
    }

    void    postDisConnect() override
    {
    }

    void    onClose() override
    {
    }

private:
    int     mFd;
};

EventLoop::EventLoop()
{
    mSelfThreadid = std::this_thread::get_id();
#ifdef PLATFORM_WINDOWS
    mPGetQueuedCompletionStatusEx = NULL;
    HMODULE kernel32_module = GetModuleHandleA("kernel32.dll");
    if (kernel32_module != NULL) {
        mPGetQueuedCompletionStatusEx = (sGetQueuedCompletionStatusEx)GetProcAddress(
            kernel32_module,
            "GetQueuedCompletionStatusEx");
    }
    mIOCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, 0, 1);
    memset(&mWakeupOvl, sizeof(mWakeupOvl), 0);
    mWakeupOvl.Offset = EventLoop::OVL_RECV;
    mWakeupChannel = new WakeupChannel(-1);
#else
    mEpollFd = epoll_create(1);
    mWakeupFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    mWakeupChannel = new WakeupChannel(mWakeupFd);
    linkChannel(mWakeupFd, mWakeupChannel);
#endif

    mIsAlreadyPostedWakeUp = false;
    mInWaitIOEvent = false;

    mEventEntries = NULL;
    mEventEntriesNum = 0;

    recalocEventSize(1024);
}

EventLoop::~EventLoop()
{
    delete mWakeupChannel;
    mWakeupChannel = nullptr;
#ifdef PLATFORM_WINDOWS
    CloseHandle(mIOCP);
    mIOCP = INVALID_HANDLE_VALUE;
#else
    close(mWakeupFd);
    mWakeupFd = -1;
    close(mEpollFd);
    mEpollFd = -1;
#endif
    delete[] mEventEntries;
    mEventEntries = nullptr;
}

void EventLoop::loop(int64_t timeout)
{
#ifndef NDEBUG
    assert(mSelfThreadid == std::this_thread::get_id());
#endif
    /*  warn::如果mAfterLoopProcs不为空（目前仅当第一次loop(时）之前就添加了回调），将timeout改为0，表示不阻塞iocp/epoll wait   */
    if (!mAfterLoopProcs.empty())
    {
        timeout = 0;
    }

    mInWaitIOEvent = true;

#ifdef PLATFORM_WINDOWS
    ULONG numComplete = 0;
    BOOL rc = mPGetQueuedCompletionStatusEx(mIOCP, mEventEntries, mEventEntriesNum, &numComplete, static_cast<DWORD>(timeout), false);
    mIsAlreadyPostedWakeUp = false;
    mInWaitIOEvent = false;

    if (rc)
    {
        for (ULONG i = 0; i < numComplete; ++i)
        {
            Channel* ds = (Channel*)mEventEntries[i].lpCompletionKey;
            if (mEventEntries[i].lpOverlapped->Offset == EventLoop::OVL_RECV)
            {
                ds->canRecv();
            }
            else if (mEventEntries[i].lpOverlapped->Offset == EventLoop::OVL_SEND)
            {
                ds->canSend();
            }
            else if (mEventEntries[i].lpOverlapped->Offset == EventLoop::OVL_CLOSE)
            {
                ds->onClose();
            }
            else
            {
                assert(false);
            }
        }
    }
#else
    int numComplete = epoll_wait(mEpollFd, mEventEntries, mEventEntriesNum, timeout);
    mIsAlreadyPostedWakeUp = false;
    mInWaitIOEvent = false;

    for (int i = 0; i < numComplete; ++i)
    {
        Channel*   ds = (Channel*)(mEventEntries[i].data.ptr);
        uint32_t event_data = mEventEntries[i].events;

        if (event_data & EPOLLRDHUP)
        {
            ds->canRecv();
            ds->onClose();   /*  无条件调用断开处理(安全的，不会造成重复close)，以防canRecv里没有recv 断开通知*/
        }
        else
        {
            if (event_data & EPOLLIN)
            {
                ds->canRecv();
            }

            if (event_data & EPOLLOUT)
            {
                ds->canSend();
            }
        }
    }
#endif

    /*
    warn::  此时mAfterLoopProcs里面可能包括了断开回调，而mAsyncProcs里也可能有异步断开请求。
    在处理mAsyncProcs的断开请求中会释放Channel。因此必须先处理mAfterLoopProcs，以执行断开回调，如果在mAsyncProcs之后处理mAfterLoopProcs，
    就会出现段错误。
    */
    processAfterLoopProcs();

    /*  执行异步请求  */
    processAsyncProcs();

    /*  warn::  理论上可以不做下面的两次mAfterLoopProcs遍历，因为loop函数之前会判断mAfterLoopProcs是否为空。如果不为空，则不会阻塞epoll或iocp，但这样直接让epoll/iocp返回可能会浪费一些CPU（需要具体测试）    */

    /*  warn::  继续处理mAfterLoopProcs，是因为在执行异步请求队列时可能会向mAfterLoopProcs添加flush send等回调  */
    processAfterLoopProcs();
    /*  warn::  继续处理mAfterLoopProcs，是因为在上面的遍历(执行回调）中可能会收集到socket 断开，则又向mAfterLoopProcs添加了断开回调函数*/
    processAfterLoopProcs();

    assert(mAfterLoopProcs.empty());

    if (numComplete == mEventEntriesNum)
    {
        /*  如果事件被填充满了，则扩大事件结果队列大小，可以让一次epoll/iocp wait获得尽可能更多的通知 */
        recalocEventSize(mEventEntriesNum + 128);
    }
}

void EventLoop::processAfterLoopProcs()
{
    copyAfterLoopProcs.swap(mAfterLoopProcs);
    for (auto& x : copyAfterLoopProcs)
    {
        x();
    }
    copyAfterLoopProcs.clear();
}

void EventLoop::processAsyncProcs()
{
    std::vector<USER_PROC> temp;
    mAsyncProcsMutex.lock();
    temp.swap(mAsyncProcs);
    mAsyncProcsMutex.unlock();

    for (auto& x : temp)
    {
        x();
    }
}

bool EventLoop::wakeup()
{
    bool ret = false;
    /*  TODO::1、保证线程安全；2、保证io线程处于wait状态时，外界尽可能少发送wakeup；3、是否io线程有必要自己向自身投递一个wakeup来唤醒下一次wait？ */
    if (mSelfThreadid != std::this_thread::get_id())
    {
        if (mInWaitIOEvent)
        {
            /*如果可能处于iocp wait则直接投递wakeup(这里可能导致多个线程重复投递了很多wakeup*/
            /*这里只能无条件投递，而不能根据mIsAlreadyPostedWakeUp标志判断，因为可能iocp wait并还没返回*/
            /*如果iocp已经wakeup，然而这里不投递的话，有可能无法唤醒下一次iocp (这不像epoll lt模式下的eventfd，只要有数据没读，则epoll总会wakeup)*/
            mIsAlreadyPostedWakeUp = true;
#ifdef PLATFORM_WINDOWS
            PostQueuedCompletionStatus(mIOCP, 0, (ULONG_PTR)mWakeupChannel, &mWakeupOvl);
#else
            uint64_t one = 1;
            ssize_t n = write(mWakeupFd, &one, sizeof one);
#endif
            ret = true;
        }
        else
        {
            /*如果一定没有处于epoll/iocp wait*/
            /*进入此else，则mIsAlreadyPostedWakeUp一定为false*/
            /*多个线程使用互斥锁判断mIsAlreadyPostedWakeUp标志，以保证不重复投递*/
            if (!mIsAlreadyPostedWakeUp)
            {
                mFlagMutex.lock();

                if (!mIsAlreadyPostedWakeUp)
                {
                    mIsAlreadyPostedWakeUp = true;
#ifdef PLATFORM_WINDOWS
                    PostQueuedCompletionStatus(mIOCP, 0, (ULONG_PTR)mWakeupChannel, &mWakeupOvl);
#else
                    uint64_t one = 1;
                    ssize_t n = write(mWakeupFd, &one, sizeof one);
#endif
                    ret = true;
                }

                mFlagMutex.unlock();
            }
        }
    }

    return ret;
}

void EventLoop::linkChannel(int fd, Channel* ptr)
{
#ifdef PLATFORM_WINDOWS
    HANDLE ret = CreateIoCompletionPort((HANDLE)fd, mIOCP, (DWORD)ptr, 0);
#else
    struct epoll_event ev = { 0, { 0 } };
    ev.events = EPOLLET | EPOLLIN | EPOLLRDHUP;
    ev.data.ptr = ptr;
    int ret = epoll_ctl(mEpollFd, EPOLL_CTL_ADD, fd, &ev);
#endif
}

void EventLoop::addChannel(int fd, Channel* c, CHANNEL_ENTER_HANDLE f)
{
    pushAsyncProc([fd, c, this, f] () {
        c->setNoBlock();
        linkChannel(fd, c);
        c->setEventLoop(this);
        f(c);
    });
}

void EventLoop::pushAsyncProc(std::function<void(void)> f)
{
    if (mSelfThreadid != std::this_thread::get_id())
    {
        /*TODO::效率是否可以优化，多个线程同时添加异步函数，加锁导致效率下降*/
        mAsyncProcsMutex.lock();
        mAsyncProcs.push_back(f);
        mAsyncProcsMutex.unlock();

        wakeup();
    }
    else
    {
        f();
    }
}

void EventLoop::pushAfterLoopProc(USER_PROC f)
{
    mAfterLoopProcs.push_back(f);
}

void EventLoop::restoreThreadID()
{
    mSelfThreadid = std::this_thread::get_id();
}

#ifdef PLATFORM_WINDOWS
HANDLE EventLoop::getIOCPHandle() const
{
    return mIOCP;
}
#else
int EventLoop::getEpollHandle() const
{
    return mEpollFd;
}
#endif

void EventLoop::recalocEventSize(int size)
{
    if (mEventEntries != NULL)
    {
        delete[] mEventEntries;
    }

#ifdef PLATFORM_WINDOWS
    mEventEntries = new OVERLAPPED_ENTRY[size];
#else
    mEventEntries = new epoll_event[size];
#endif

    mEventEntriesNum = size;
}