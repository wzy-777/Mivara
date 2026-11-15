/**
 * KVM_EVENT_QUEUE.H – SPSC queue between a platform input thread and the
 * stitch socket drain thread.
 *
 * Mouse moves are coalesced on the consumer side: a run of pending moves
 * collapses to the newest one, so a 1 kHz mouse cannot grow the queue without
 * bound. The producer must never touch a published slot — an in-place
 * "overwrite the newest" optimisation races with the drain thread copying that
 * same slot (the consumer can advance tail between the producer's tail load
 * and its write, tearing a coordinate pair).
 *
 * Used by the Windows hook backend (kvm.cpp) and the Linux XInput2 backend
 * (kvm_x11_ctl.cpp).
 */
#ifndef MIVARA_KVM_EVENT_QUEUE_H
#define MIVARA_KVM_EVENT_QUEUE_H

#include <atomic>

#include "kvm.h"

class KvmEventQueue {
public:
    /* Large enough for one real drag image (DROP_DRAG_IMAGE_MAX_PIXELS * 4 /
     * 12 frames) plus input headroom. The batch drain sends it in one pass,
     * so this is capacity, not a latency cost. */
    static const unsigned CAP = 16384;

    bool push(const KvmEvent &e)
    {
        const unsigned h = head.load(std::memory_order_relaxed);
        const unsigned t = tail.load(std::memory_order_acquire);
        if (h - t >= CAP) return false;
        buf[h % CAP] = e;
        head.store(h + 1, std::memory_order_release);
        return true;
    }

    bool pushMove(uint32_t x, uint32_t y)
    {
        /* Keep headroom for key/button events. While the drain thread is
         * blocked in a slow send, a 1-4 kHz mouse would otherwise fill the
         * ring with moves and silently discard a key-up / button-up queued
         * behind it, leaving the peer's Ctrl or left button held down. Moves
         * are lossy by design (pop() already keeps only the newest of a run),
         * key and button events are not. */
        const unsigned h = head.load(std::memory_order_relaxed);
        const unsigned t = tail.load(std::memory_order_acquire);
        if (h - t >= CAP - 512) return false;
        KvmEvent e = {};
        e.type = KVM_EV_MOVE;
        e.a = x;
        e.b = y;
        buf[h % CAP] = e;
        head.store(h + 1, std::memory_order_release);
        return true;
    }

    /* Free ring slots. The producer is a single thread, so a caller that
     * checks this and then pushes the same number of events cannot be raced
     * by another producer. Used to keep a burst of visual-only icon frames
     * from crowding out a key/button event. */
    unsigned freeSlots() const
    {
        const unsigned h = head.load(std::memory_order_relaxed);
        const unsigned t = tail.load(std::memory_order_acquire);
        return CAP - (h - t);
    }

    /* Conservative snapshot used to skip the select() wait before a drain. */
    bool empty() const
    {
        return head.load(std::memory_order_acquire) ==
               tail.load(std::memory_order_relaxed);
    }

    bool pop(KvmEvent &e)
    {
        unsigned t = tail.load(std::memory_order_relaxed);
        const unsigned h = head.load(std::memory_order_acquire);
        if (t == h) return false;
        e = buf[t % CAP];
        ++t;
        /* Consumer side is the only tail writer, so reading past t up to h is
         * safe; keep the last move of a contiguous run. */
        while (t != h) {
            const KvmEvent &n = buf[t % CAP];
            if (e.type != KVM_EV_MOVE || n.type != KVM_EV_MOVE) break;
            e = n;
            ++t;
        }
        tail.store(t, std::memory_order_release);
        return true;
    }

    /* Drop events left by a previous session. Only safe while no hooks/grabs
     * are installed and the drain thread is not running (session start). A
     * reconnect must never replay a stale button-up/key-up against the new
     * session. */
    void reset()
    {
        head.store(0, std::memory_order_relaxed);
        tail.store(0, std::memory_order_relaxed);
    }

private:
    KvmEvent buf[CAP] = {};
    std::atomic<unsigned> head{0};
    std::atomic<unsigned> tail{0};
};

#endif /* MIVARA_KVM_EVENT_QUEUE_H */
