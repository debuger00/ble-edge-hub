// C++17: g++ -std=c++17 -Wall -Wextra -Wpedantic -pthread 00mutex_demo.cpp -o mutex_demo
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>

std::queue<int> q;
std::mutex mtx;  // 同时保护 q 和 finished
std::condition_variable cv;
bool finished = false;  // 所有生产者是否已经结束

void producer(int id) {
  for (int i = 0; i < 5; ++i) {
    const int value = id * 100 + i;
    {
      std::lock_guard<std::mutex> lock(mtx);  // 加锁
      // 临界区：本例将日志也放在锁内，方便观察入队/出队顺序
      q.push(value);
      std::cout << "Producer " << id << " push: " << value << '\n';
    }  // 自动解锁

    cv.notify_one();  // 提醒消费者重新检查条件
    // 模拟生产耗时；不持有锁，也不靠 sleep 保证正确性
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
  }
}

void consumer() {
  int consumed = 0;  // 只有本线程使用，不需要锁
  while (true) {
    int value;
    {
      std::unique_lock<std::mutex> lock(mtx);  // 加锁
      cv.wait(lock, [] { return !q.empty() || finished; });
      // 等待期间释放锁；返回前重新获得锁，且条件为真

      if (q.empty()) {
        // 此时必然 finished == true，且剩余数据已经取完
        break;  // 离开作用域，仍然会自动解锁
      }

      value = q.front();
      q.pop();
      std::cout << "Consumer pop: " << value << '\n';
    }  // 自动解锁

    // 在锁外处理 value，模拟业务耗时
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    ++consumed;
  }

  // 走到这里时生产者已结束，且不会再有其他线程输出
  std::cout << "Consumer finished. Total: " << consumed << '\n';
}

int main() {
  std::thread t3(consumer);
  std::thread t1(producer, 1);
  std::thread t2(producer, 2);

  t1.join();
  t2.join();

  {
    std::lock_guard<std::mutex> lock(mtx);
    finished = true;
  }
  cv.notify_all();  // 即使队列为空，也唤醒消费者检查结束状态

  t3.join();
  std::cout << "Main finished.\n";
}
