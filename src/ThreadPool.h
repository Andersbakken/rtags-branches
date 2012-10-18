#ifndef THREADPOOL_H
#define THREADPOOL_H

#include "Mutex.h"
#include "WaitCondition.h"
#include <deque>

class ThreadPoolThread;

class ThreadPool
{
public:
    ThreadPool(int concurrentJobs);
    ~ThreadPool();

    void setConcurrentJobs(int concurrentJobs);
    void clearBackLog();

    class Job
    {
    public:
        Job();
        virtual ~Job();

    protected:
        virtual void run() {}
    private:
        int mPriority;
        Mutex mMutex;

        friend class ThreadPool;
        friend class ThreadPoolThread;
    };

    enum { Guaranteed = -1 };

    void start(const SharedPtr<Job> &job, int priority = 0);

    static int idealThreadCount();
    static ThreadPool* globalInstance();

private:
    static bool jobLessThan(const SharedPtr<Job> &l, const SharedPtr<Job> &r);

private:
    int mConcurrentJobs;
    Mutex mMutex;
    WaitCondition mCond;
    std::deque<SharedPtr<Job> > mJobs;
    List<ThreadPoolThread*> mThreads;
    int mBusyThreads;

    static ThreadPool* sGlobalInstance;

    friend class ThreadPoolThread;
};

#endif
