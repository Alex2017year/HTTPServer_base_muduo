
#ifndef __STDC_LIMIT_MACROS
#define __STDC_LIMIT_MACROS
#endif

#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/Timer.h>
#include <muduo/net/TimerId.h>
#include <muduo/net/TimerQueue.h>
#include <sys/timerfd.h>
#include <functional>
#include <algorithm>

namespace muduo {
namespace net {
namespace detail {

int createTimerfd() {
  int timerfd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (timerfd < 0) {
    LOG_SYSFATAL << "Failed in timerfd_create";
  }
  return timerfd;
}

struct timespec howMuchTimeFromNow(Timestamp when) {
  int64_t microseconds =
      when.microSecondsSinceEpoch() - Timestamp::now().microSecondsSinceEpoch();
  if (microseconds < 100) microseconds = 100;

  struct timespec ts;
  ts.tv_sec =
      static_cast<time_t>(microseconds / Timestamp::kMicroSecondsPerSecond);
  ts.tv_nsec = static_cast<long>(
      (microseconds % Timestamp::kMicroSecondsPerSecond) * 1000);

  return ts;
}

void readTimerfd(int timerfd, Timestamp now) {
  uint64_t howmany;
  ssize_t n = ::read(timerfd, &howmany, sizeof howmany);
  LOG_TRACE << "TimerQueue::handleRead() " << howmany << " at "
            << now.toString();
  if (n != sizeof howmany) {
    LOG_ERROR << "TimerQueue::handleRead() reads " << n
              << " bytes instead of 8";
  }
}

void resetTimerfd(int timerfd, Timestamp expiration) {
  // wake up loop by timerfd_settime()
  struct itimerspec newValue;
  struct itimerspec oldValue;
  bzero(&newValue, sizeof newValue);
  bzero(&oldValue, sizeof oldValue);
  newValue.it_value = howMuchTimeFromNow(expiration);
  int ret = ::timerfd_settime(timerfd, 0, &newValue, &oldValue);
  if (ret) {
    LOG_SYSERR << "timerfd_settime()";
  }
}

}  // detail
}  // net
}  // muduo
using namespace muduo;
using namespace muduo::net;
using namespace muduo::net::detail;

TimerQueue::TimerQueue(EventLoop* loop)
    : loop_(loop),
      timerfd_(createTimerfd()),
      timerfdChannel_(loop, timerfd_),
      timers_() {
  //!!!set the callback;
  timerfdChannel_.setReadCallback(
        std::bind(&TimerQueue::handleRead,this));
  // we are always reading the timerfd, we disarm it with timerfd_settime.
  timerfdChannel_.enableReading();

}

TimerQueue::~TimerQueue() {
  ::close(timerfd_);
  // do not remove channel, since we're in EventLoop::dtor();
  // do not need to delete ,since we're using unique_ptr;
  // for (TimerList::iterator it = timers_.begin(); it != timers_.end(); ++it) {
  //   delete it->second;
  // }
}
TimerId TimerQueue::addTimer(const TimerCallback&& cb,
							 Timestamp when,
							 double interval)
{
	TimerPtr timer(new Timer(std::move(cb),when,interval));
	loop_->runInLoop(
      std::bind(&TimerQueue::addTimerInLoop,this,timer));
	return TimerId(timer);
}
void TimerQueue::addTimerInLoop(TimerPtr timer)
{
  loop_->assertInLoopThread();
  bool earliestChanged = insert(timer);

  if(earliestChanged)
  {
    //if the timer to add is the earliest to be triggerd,
    //then modify the current trigger time
    resetTimerfd(timerfd_,timer->expiration());
  }

}

void TimerQueue::cancelInLoop(TimerId timerId)
{
  loop_->assertInLoopThread();
  TimerPtr canceltimer = timerId.value_.lock();
  if(canceltimer)
  {
    Entry timerEntry(canceltimer->expiration(),canceltimer);
    TimerList::iterator it = timers_.find(timerEntry);
    if(it != timers_.end())
    {
      size_t n = timers_.erase(Entry(canceltimer->expiration(), canceltimer));
      assert(n == 1); (void)n;
    }
    else if(callingExpiredTimers_)
    {
      LOG_INFO << "self cancel!!!" ;
      cancelingTimers_.insert(timerEntry);
    }
  }
  else
  {
    LOG_DEBUG << "cancel expired timer" ;
  }
}


void TimerQueue::cancel(TimerId timerId)
{
  loop_->runInLoop(
      std::bind(&TimerQueue::cancelInLoop, this, timerId));
}


void TimerQueue::handleRead()
{
  LOG_DEBUG << "TimerQueue::handleRead()" ;
  loop_->assertInLoopThread();
  Timestamp now(Timestamp::now());
  readTimerfd(timerfd_, now);

  std::vector<Entry> expired = getExpired(now);
  callingExpiredTimers_ = true;
  cancelingTimers_.clear();
  // safe to callback outside critical section
  for (std::vector<Entry>::iterator it = expired.begin();
      it != expired.end(); ++it)
  {
    it->second->run();
  }
  callingExpiredTimers_ =false;
  reset(expired, now);
}


std::vector<TimerQueue::Entry> TimerQueue::getExpired(Timestamp now)
{
  std::vector<Entry> expired;
  TimerPtr PTR_MAX(NULL);
  Entry sentry = std::make_pair(now,PTR_MAX);
  //smaller than now
  //what will be ,when Timestamp now equals
  //TimerList::iterator it = timers_.lower_bound(sentry);
  //to change std::lower_bound, to handle the equal case
  //use the lambda 
  TimerList::iterator it = std::lower_bound(
              timers_.begin(),
              timers_.end(),
              sentry,
              [](const Entry& a,const Entry& b)
              {return a.first <= b.first;});
  //check condition
  assert(it == timers_.end() || now < it->first);
  std::copy(timers_.begin(), it, back_inserter(expired));
  timers_.erase(timers_.begin(), it);
  return expired;
}

void TimerQueue::reset(const std::vector<Entry>& expired, Timestamp now)
{
  Timestamp nextExpire;

  for (std::vector<Entry>::const_iterator it = expired.begin();
      it != expired.end(); ++it)
  {
    if (it->second->repeat() && cancelingTimers_.find(*it) == cancelingTimers_.end())
    {
      it->second->restart(now);
      insert(it->second);
    }
    else
    {
      // FIXME move to a free list
      //delete it->second;
    }
  }

  if (!timers_.empty())
  {
    nextExpire = timers_.begin()->second->expiration();
  }

  if (nextExpire.valid())
  {
    resetTimerfd(timerfd_, nextExpire);
  }
}


bool TimerQueue::insert(TimerPtr timer)
{
	bool earliestChanged = false;
	Timestamp when = timer->expiration();
	TimerList::iterator it = timers_.begin();// set container
	if(it == timers_.end() || when < it->first)
	{
		earliestChanged = true;
	}
	std::pair<TimerList::iterator,bool> result = 
			timers_.insert(std::make_pair(when,timer));
	assert(result.second);(void)result;
	return earliestChanged;
}




