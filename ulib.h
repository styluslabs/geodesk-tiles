#pragma once

// cut and paste from styluslabs/ulib/threadutil.h

// thread pool based on github.com/progschj/ThreadPool

#include <vector>
#include <queue>
#include <memory>
#include <thread>
#include <future>
#include <functional>

class ThreadPool
{
public:
  ThreadPool(size_t nthreads);
  template<class F, class... Args>
  auto enqueue(F&& f, Args&&... args) -> std::future<typename std::result_of<F(Args...)>::type>;
  void waitForIdle();
  ~ThreadPool();

private:
  std::vector< std::thread > workers;
  std::queue< std::function<void()> > tasks;

  std::mutex queue_mutex;
  std::condition_variable queue_cv;
  std::condition_variable idle_cv;
  bool stop = false;
  int n_running = 0;
};

// the constructor just launches some amount of workers
inline ThreadPool::ThreadPool(size_t nthreads)
{
  if(nthreads == 0)
    nthreads = std::thread::hardware_concurrency();
  n_running = nthreads;  // once all threads start and enter wait, n_running will be zero
  for(size_t ii = 0; ii < nthreads; ++ii) {
    workers.emplace_back([this](){
      for(;;) {
        std::function<void()> task;
        {
          std::unique_lock<std::mutex> lock(queue_mutex);
          --n_running;
          idle_cv.notify_all();
          queue_cv.wait(lock, [this](){ return stop || !tasks.empty(); });
          if(stop && tasks.empty())
            return;
          task = std::move(tasks.front());
          tasks.pop();
          ++n_running;
        }
        task();
      }
    });
  }
}

// add new work item to the pool - returns a std::future, which has a wait() method
template<class F, class... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args) -> std::future<typename std::result_of<F(Args...)>::type>
{
  using return_type = typename std::result_of<F(Args...)>::type;

  auto task = std::make_shared< std::packaged_task<return_type()> >(
    std::bind(std::forward<F>(f), std::forward<Args>(args)...)
  );

  std::future<return_type> res = task->get_future();
  {
    std::unique_lock<std::mutex> lock(queue_mutex);
    if(!stop)
      tasks.emplace([task](){ (*task)(); });
  }
  queue_cv.notify_one();
  return res;
}

inline void ThreadPool::waitForIdle()
{
  std::unique_lock<std::mutex> lock(queue_mutex);
  //notify_task_finish = true;
  idle_cv.wait(lock, [&](){ return tasks.empty() && n_running == 0; });
  //notify_task_finish = false;
}

// the destructor joins all threads
inline ThreadPool::~ThreadPool()
{
  {
    std::unique_lock<std::mutex> lock(queue_mutex);
    stop = true;
  }
  queue_cv.notify_all();
  for(std::thread &worker: workers)
    worker.join();
}
