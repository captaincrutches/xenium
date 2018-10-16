#ifndef XENIUM_RAMALHETE_QUEUE_HPP
#define XENIUM_RAMALHETE_QUEUE_HPP

#include <xenium/acquire_guard.hpp>
#include <xenium/backoff.hpp>
#include <xenium/reclamation/detail/marked_ptr.hpp>

#include <atomic>
#include <stdexcept>

namespace xenium {

/**
 * @brief A fast unbounded lock-free multi-producer/multi-consumer FIFO queue.
 * 
 * This is an implementation of the `FAAArrayQueue` by Ramalhete and Correia.
 * A description of the algorithm can be found here:
 * http://concurrencyfreaks.blogspot.com/2016/11/faaarrayqueue-mpmc-lock-free-queue-part.html
 * 
 * It is faster and more efficient than the `michael_scott_queue`, but less generic as it can
 * only handle pointers to instances of `T`.
 *
 * @tparam T
 * @tparam Reclaimer the reclamation scheme to use for internally created nodes.
 * @tparam SlotsPerNode the number of slots per node; defaults to 1024.
 * @tparam Backoff the backoff stragtey to be used; defaults to `no_backoff`.
 */
template <class T, class Reclaimer, unsigned SlotsPerNode = 1024, class Backoff = no_backoff>
class ramalhete_queue {
public:
  static_assert(SlotsPerNode > 0, "SlotsPerNode must be greater than zero");

  using value_type = T*;

  ramalhete_queue();
  ~ramalhete_queue();

  /**
   * Enqueues the given value to the queue.
   * This operation might have to allocate a new node.
   * Progress guarantees: lock-free (may perform a memory allocation)
   * @param value
   */
  void enqueue(value_type value);

  /**
   * Tries to dequeue an object from the queue.
   * Progress guarantees: lock-free
   * @param result
   * @return `true` if the operation was successful, otherwise `false`
   */
  bool try_dequeue(value_type& result);

private:
  struct node;

  using concurrent_ptr = typename Reclaimer::template concurrent_ptr<node, 0>;
  using marked_ptr = typename concurrent_ptr::marked_ptr;
  using guard_ptr = typename concurrent_ptr::guard_ptr;

  using marked_value = reclamation::detail::marked_ptr<T, 1>;

  struct node : Reclaimer::template enable_concurrent_ptr<node> {
    std::atomic<unsigned>     dequeue_idx;
    std::atomic<marked_value> items[SlotsPerNode];
    std::atomic<unsigned>     enqueue_idx;
    concurrent_ptr next;

    // Start with the first entry pre-filled
    node(T* item) :
      dequeue_idx{0},
      enqueue_idx{1},
      next{nullptr}
    {
      items[0].store(item, std::memory_order_relaxed);
      for (unsigned i = 1; i < SlotsPerNode; i++)
        items[i].store(nullptr, std::memory_order_relaxed);
    }
  };

  alignas(64) concurrent_ptr head;
  alignas(64) concurrent_ptr tail;
};

template <class T, class Reclaimer, unsigned SlotsPerNode, class Backoff>
ramalhete_queue<T, Reclaimer, SlotsPerNode, Backoff>::ramalhete_queue()
{
  auto n = new node(nullptr);
  n->enqueue_idx.store(0, std::memory_order_relaxed);
  head.store(n, std::memory_order_relaxed);
  tail.store(n, std::memory_order_relaxed);
}

template <class T, class Reclaimer, unsigned SlotsPerNode, class Backoff>
ramalhete_queue<T, Reclaimer, SlotsPerNode, Backoff>::~ramalhete_queue()
{
  // (1) - this acquire-load synchronizes-with the release-CAS (11)
  auto n = head.load(std::memory_order_acquire);
  while (n)
  {
    // (2) - this acquire-load synchronizes-with the release-CAS (4)
    auto next = n->next.load(std::memory_order_acquire);
    delete n.get();
    n = next;
  }
}

template <class T, class Reclaimer, unsigned SlotsPerNode, class Backoff>
void ramalhete_queue<T, Reclaimer, SlotsPerNode, Backoff>::enqueue(value_type value)
{
  if (value == nullptr)
    throw std::invalid_argument("value can not be nullptr");

  Backoff backoff;

  guard_ptr t;
  for (;;) {
    // (3) - this acquire-load synchronizes-with the release-CAS (5, 7)
    t.acquire(tail, std::memory_order_acquire);

    const int idx = t->enqueue_idx.fetch_add(1, std::memory_order_relaxed);
    if (idx > SlotsPerNode - 1)
    {
      // This node is full
      if (t != tail.load(std::memory_order_relaxed))
        continue; // some other thread already added a new node.

      auto next = t->next.load(std::memory_order_relaxed);
      if (next == nullptr)
      {
        node* new_node = new node(value);
        marked_ptr expected = nullptr;
        // (4) - this release-CAS synchronizes-with the acquire-load (2, 6, 10)
        if (t->next.compare_exchange_strong(expected, new_node,
                                            std::memory_order_release,
                                            std::memory_order_relaxed))
        {
          expected = t;
          // (5) - this release-CAS synchronizes-with the acquire-load (3)
          tail.compare_exchange_strong(expected, new_node, std::memory_order_release, std::memory_order_relaxed);
          return;
        }
        // some other node already added a new node
        delete new_node;
      } else {
        // (6) - this acquire-load synchronizes-with the release-CAS (4)
        next = t->next.load(std::memory_order_acquire);
        marked_ptr expected = t;
        // (7) - this release-CAS synchronizes-with the acquire-load (3)
        tail.compare_exchange_strong(expected, next, std::memory_order_release, std::memory_order_relaxed);
      }
      continue;
    }

    marked_value expected = nullptr;
    // (8) - this release-CAS synchronizes-with the acquire-exchange (12)
    if (t->items[idx].compare_exchange_strong(expected, value, std::memory_order_release, std::memory_order_relaxed))
      return;

    backoff();
  }
}

template <class T, class Reclaimer, unsigned SlotsPerNode, class Backoff>
bool ramalhete_queue<T, Reclaimer, SlotsPerNode, Backoff>::try_dequeue(value_type& result)
{
  Backoff backoff;

  guard_ptr h;
  for (;;) {
    // (9) - this acquire-load synchronizes-with the release-CAS (11)
    h.acquire(head, std::memory_order_acquire);

    if (h->dequeue_idx.load(std::memory_order_relaxed) >= h->enqueue_idx.load(std::memory_order_relaxed) &&
        h->next.load(std::memory_order_relaxed) == nullptr)
      break;

    const int idx = h->dequeue_idx.fetch_add(1, std::memory_order_relaxed);
    if (idx > SlotsPerNode - 1)
    {
      // This node has been drained, check if there is another one
      // (10) - this acquire-load synchronizes-with the release-CAS (4)
      auto next = h->next.load(std::memory_order_acquire);
      if (next == nullptr)
        break;  // No more nodes in the queue

      marked_ptr expected = h;
      // (11) - this release-CAS synchronizes-with the acquire-load (1, 9)
      if (head.compare_exchange_strong(expected, next, std::memory_order_release, std::memory_order_relaxed))
        h.reclaim(); // The old node has been unlinked -> reclaim it.

      continue;
    }

    // (12) - this acquire-exchange synchronizes-with the release-CAS (8)
    auto value = h->items[idx].exchange(marked_value(nullptr, 1), std::memory_order_acquire);
    if (value != nullptr) {
      result = value.get();
      return true;
    }

    backoff();
  }

  return false;
}

}

#endif
