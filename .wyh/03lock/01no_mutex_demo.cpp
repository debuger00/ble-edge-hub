// g++ -std=c++17 -O0 -g -Wall -Wextra -Wpedantic -pthread 01no_mutex_demo.cpp -o 01no_mutex_demo
#include <atomic>
#include <chrono>
#include <iostream>
#include <queue>
#include <thread>

std::queue<int> q;  // 故意不加锁：存在数据竞争

// 仅用于通知生产结束，不保护 q
std::atomic<bool> finished{false};

constexpr int ITEMS_PER_PRODUCER = 2000;  //这里只有五次的化就不太好看出问题， 数字大一点后面出错概率就高一点

void producer(int id) {
  for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
    const int value = id * 10000 + i;

    // 没有互斥锁保护
    q.push(value);

    std::cout << "Producer " << id << " push: " << value << '\n';

    std::this_thread::sleep_for(std::chrono::milliseconds(80));
  }
}

void consumer() {
  int consumed = 0;

  while (true) {
    // 先读取结束标志
    const bool production_done = finished.load();

    // 没有互斥锁保护
    if (!q.empty()) {
      int value = q.front();
      q.pop();

      std::cout << "Consumer pop: " << value << '\n';

      ++consumed;

      std::this_thread::sleep_for(std::chrono::milliseconds(120));
    } else if (production_done) {
      // 按设计意图：生产结束且队列为空时退出
      break;
    } else {
      // 没有条件变量，暂时休眠后再检查
      // 休眠不能保证线程安全
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  std::cout << "Consumer finished. Total: " << consumed << '\n';
}

int main() {
  std::thread t3(consumer);
  std::thread t1(producer, 1);
  std::thread t2(producer, 2);

  t1.join();
  t2.join();

  finished.store(true);

  t3.join();

  std::cout << "Expected total: " << 2 * ITEMS_PER_PRODUCER << '\n';

  std::cout << "Main finished.\n";
}