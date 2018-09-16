#ifndef _SEMAPHORE_H_
#define _SEMAPHORE_H_

#include "adb_struct.h"
#include <boost/thread/locks.hpp>
#include <boost/thread/condition_variable.hpp>	//线程同步

//信号量实现 
class Semaphore {
public:
	Semaphore(long count = 0)
		: count_(count){
	}

	void Signal(size_t n = 1) {//默认自增1（计数） 或者 Semaphore_Bool_Open | Semaphore_Bool_Close  (开关)
		{	boost::unique_lock<boost::mutex> lock(mutex_);
			count_ += n;}
		cv_.notify_one();
	}

	void Wait() {
		boost::unique_lock<boost::mutex> lock(mutex_);
		if (count_ < 1){
			cv_.wait(lock);
		}
		--count_;
	}

	bool Wait_time(int w_time){
		boost::unique_lock<boost::mutex> lock(mutex_);
		if (count_ < 1){
			if (cv_.timed_wait(lock, boost_second_get(w_time))){
				--count_;
				return true;
			}
			return false;
		}
		--count_;
		return true;
	}
	inline	void reset(){ count_ = 0; }
private:
	boost::mutex mutex_;
	boost::condition_variable cv_;
	int count_;
};

//通过shared_lock 实现广播功能
class Radiophore {
public:
	Radiophore()
	{}
	void Signal() {
		cv_any.notify_all();
	}

	void Wait() {
		boost::shared_lock<boost::shared_mutex> _slock(mutex_s);
		cv_any.wait(_slock);
	}

	bool Wait_time(int w_time = 1){
		boost::shared_lock<boost::shared_mutex> _slock(mutex_s);
		return cv_any.timed_wait(_slock, boost_second_get(w_time));
	}
private:
	boost::shared_mutex mutex_s;
	boost::condition_variable_any cv_any;
};

#endif	//_SEMAPHORE_H_