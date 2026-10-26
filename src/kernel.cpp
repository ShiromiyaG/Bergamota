// The kernel's threads, objects and waits. Translated from Zakuro's kernel
// (Rust, MIT, Copyright (c) 2026 fearkov; see LICENSE-zakuro).

#include "runtime/kernel.h"

#include <algorithm>

namespace runtime {

uint64_t nanos_to_ticks(uint64_t nanos) {
    // in two steps, so that multi-second timeouts stay inside 64 bits
    return (nanos / 1000000000) * CPU_CLOCK_HZ + (nanos % 1000000000) * CPU_CLOCK_HZ / 1000000000;
}

Kernel::Kernel(GuestMemory &memory, Clock &clock, Hooks &hooks) : memory_(memory), clock_(clock), hooks_(hooks) {}

ThreadId Kernel::start(uint32_t entry, uint32_t stack_top, uint32_t priority) {
    auto thread = std::make_unique<Thread>();
    ThreadId id = static_cast<ThreadId>(threads_.size());
    thread->id = id;
    thread->priority = priority;
    thread->tls = hooks_.allocate_tls();
    Context &c = thread->context;
    c.r[13] = stack_top;
    c.r[14] = THREAD_EXIT;
    c.r[15] = entry & ~1u;
    c.thumb = entry & 1;
    c.tls = thread->tls;
    threads_.push_back(std::move(thread));
    Thread &t = *threads_.back();
    t.context.vfp = t.vfp;
    t.context.fpscr = &t.fpscr;
    hooks_.prepare(t.context);
    return id;
}

Thread *Kernel::current() { return current_ ? threads_[*current_].get() : nullptr; }

ObjectId Kernel::insert(KObject object) {
    Slot slot{std::move(object), 0};
    if (!free_.empty()) {
        ObjectId id = free_.back();
        free_.pop_back();
        slots_[id] = std::move(slot);
        return id;
    }
    slots_.push_back(std::move(slot));
    return static_cast<ObjectId>(slots_.size() - 1);
}

KObject *Kernel::object(ObjectId id) {
    if (id >= slots_.size() || !slots_[id]) {
        return nullptr;
    }
    return &slots_[id]->object;
}

void Kernel::add_ref(ObjectId id) {
    if (id < slots_.size() && slots_[id]) {
        slots_[id]->refcount++;
    }
}

void Kernel::release(ObjectId id) {
    if (id >= slots_.size() || !slots_[id]) {
        return;
    }
    if (slots_[id]->refcount > 0) {
        slots_[id]->refcount--;
    }
    if (slots_[id]->refcount == 0) {
        slots_[id].reset();
        free_.push_back(id);
    }
}

Handle Kernel::create_handle(ObjectId object) {
    Handle handle = next_handle_++;
    handles_[handle] = object;
    add_ref(object);
    return handle;
}

std::optional<ObjectId> Kernel::resolve(Handle handle) {
    auto it = handles_.find(handle);
    if (it == handles_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::pair<ObjectId, Handle> Kernel::create_event(ResetType reset) {
    ObjectId id = insert(Event{reset});
    return {id, create_handle(id)};
}

void Kernel::sleep(Thread &t, uint64_t ticks) {
    end_wait(t);
    t.wakeup_at = clock_.ticks() + ticks;
    t.status = ThreadStatus::Sleeping;
}

void Kernel::signal_event(ObjectId object) {
    auto *event = std::get_if<Event>(this->object(object));
    if (!event) {
        return;
    }
    if (event->reset == ResetType::Pulse) {
        // a pulse wakes what waits on it now and is gone after, lost when
        // nothing does
        bool waited = std::any_of(threads_.begin(), threads_.end(), [&](const auto &t) {
            return t->status == ThreadStatus::WaitSync &&
                   std::find(t->wait_objects.begin(), t->wait_objects.end(), object) != t->wait_objects.end();
        });
        if (!waited) {
            return;
        }
        pulsed_.push_back(object);
    }
    event->signaled = true;
}

bool Kernel::is_signaled(ObjectId object, ThreadId waiter) {
    KObject *o = this->object(object);
    if (!o) {
        return true;
    }
    if (auto *e = std::get_if<Event>(o)) {
        return e->signaled;
    }
    if (auto *s = std::get_if<Semaphore>(o)) {
        return s->count > 0;
    }
    if (auto *m = std::get_if<Mutex>(o)) {
        return !m->owner || *m->owner == waiter;
    }
    if (auto *t = std::get_if<ThreadObject>(o)) {
        return threads_[t->id]->status == ThreadStatus::Dead;
    }
    // sessions are answered before the syscall returns: never waited on
    return true;
}

void Kernel::acquire(ObjectId object, ThreadId waiter) {
    KObject *o = this->object(object);
    if (!o) {
        return;
    }
    if (auto *e = std::get_if<Event>(o)) {
        if (e->reset == ResetType::OneShot) {
            e->signaled = false;
        }
    } else if (auto *s = std::get_if<Semaphore>(o)) {
        s->count--;
    } else if (auto *m = std::get_if<Mutex>(o)) {
        m->owner = waiter;
        m->lock_count++;
    }
}

void Kernel::begin_wait(std::vector<ObjectId> objects, bool wait_all, std::optional<uint64_t> timeout) {
    Thread &t = *current();
    // what a thread waits on stays alive while it does
    for (ObjectId o : objects) {
        add_ref(o);
    }
    t.wait_objects = std::move(objects);
    t.wait_all = wait_all;
    t.wakeup_at = timeout ? std::optional<uint64_t>(clock_.ticks() + *timeout) : std::nullopt;
    t.wait_result.reset();
    t.status = ThreadStatus::WaitSync;
}

void Kernel::end_wait(Thread &t) {
    for (ObjectId o : t.wait_objects) {
        release(o);
    }
    t.wait_objects.clear();
    t.wait_all = false;
    t.wakeup_at.reset();
    t.wait_address.reset();
}

bool Kernel::try_satisfy_wait(Thread &t, uint64_t now) {
    bool timed_out = t.wakeup_at && now >= *t.wakeup_at;
    switch (t.status) {
    case ThreadStatus::Sleeping:
        if (timed_out) {
            end_wait(t);
            t.status = ThreadStatus::Ready;
            return true;
        }
        return false;
    case ThreadStatus::WaitSync: {
        std::optional<uint32_t> index;
        size_t n = t.wait_objects.size();
        if (t.wait_all) {
            bool all = true;
            for (size_t k = 0; k < n && all; k++) {
                all = is_signaled(t.wait_objects[k], t.id);
            }
            if (all) {
                index = 0;
            }
        } else {
            for (size_t k = 0; k < n; k++) {
                if (is_signaled(t.wait_objects[k], t.id)) {
                    index = static_cast<uint32_t>(k);
                    break;
                }
            }
        }
        if (index) {
            if (t.wait_all) {
                for (ObjectId o : t.wait_objects) {
                    acquire(o, t.id);
                }
            } else {
                acquire(t.wait_objects[*index], t.id);
            }
            end_wait(t);
            t.wait_result = index;
            t.status = ThreadStatus::Ready;
            return true;
        }
        if (timed_out) {
            end_wait(t);
            t.wait_result = std::optional<uint32_t>{};
            t.status = ThreadStatus::Ready;
            return true;
        }
        return false;
    }
    case ThreadStatus::WaitArbiter:
        // a signal releases these; only the timeout is checked here
        if (timed_out) {
            end_wait(t);
            t.wait_result = std::optional<uint32_t>{};
            t.status = ThreadStatus::Ready;
            return true;
        }
        return false;
    default:
        return false;
    }
}

std::optional<ThreadId> Kernel::pick_next() const {
    std::optional<uint32_t> best;
    for (const auto &t : threads_) {
        if (t->status == ThreadStatus::Ready || t->status == ThreadStatus::Running) {
            best = best ? std::min(*best, t->priority) : t->priority;
        }
    }
    if (!best) {
        return std::nullopt;
    }
    // the running thread keeps the core unless something strictly more
    // urgent is ready
    if (current_) {
        const Thread &t = *threads_[*current_];
        if (t.status == ThreadStatus::Running && t.priority <= *best) {
            return current_;
        }
    }
    // round robin within the priority, from the thread after the current one
    size_t count = threads_.size();
    size_t start = current_ ? *current_ + 1 : 0;
    for (size_t offset = 0; offset < count; offset++) {
        const Thread &t = *threads_[(start + offset) % count];
        if ((t.status == ThreadStatus::Ready || t.status == ThreadStatus::Running) && t.priority == *best) {
            return t.id;
        }
    }
    return std::nullopt;
}

void Kernel::apply_wait_result(Thread &t) {
    if (!t.wait_result || t.wait_syscall == WaitSyscall::None) {
        return;
    }
    Context &c = t.context;
    if (!*t.wait_result) {
        c.r[0] = t.wait_syscall == WaitSyscall::SleepThread ? result::SUCCESS : result::TIMEOUT;
    } else {
        c.r[0] = result::SUCCESS;
        if (t.wait_syscall == WaitSyscall::WaitSynchronizationN) {
            c.r[1] = **t.wait_result;
        }
    }
    t.wait_result.reset();
    t.wait_syscall = WaitSyscall::None;
}

bool Kernel::schedule() {
    uint64_t now = clock_.ticks();
    for (auto &t : threads_) {
        try_satisfy_wait(*t, now);
    }
    for (ObjectId id : pulsed_) {
        if (auto *e = std::get_if<Event>(object(id))) {
            e->signaled = false;
        }
    }
    pulsed_.clear();

    std::optional<ThreadId> next = pick_next();
    if (current_ && (!next || *next != *current_)) {
        Thread &t = *threads_[*current_];
        if (t.status == ThreadStatus::Running) {
            t.status = ThreadStatus::Ready;
        }
    }
    current_ = next;
    if (!next) {
        return false;
    }
    Thread &t = *threads_[*next];
    t.status = ThreadStatus::Running;
    apply_wait_result(t);
    return true;
}

std::optional<uint64_t> Kernel::next_wakeup() const {
    std::optional<uint64_t> best;
    for (const auto &t : threads_) {
        if (t->wakeup_at && (t->status == ThreadStatus::Sleeping || t->status == ThreadStatus::WaitSync ||
                             t->status == ThreadStatus::WaitArbiter)) {
            best = best ? std::min(*best, *t->wakeup_at) : *t->wakeup_at;
        }
    }
    return best;
}

void Kernel::end_thread(Thread &t) {
    end_wait(t);
    t.status = ThreadStatus::Dead;
    t.wait_result.reset();
    t.wait_syscall = WaitSyscall::None;
    // the mutexes it holds go to whoever waits on them next, as on the 3DS
    for (auto &slot : slots_) {
        if (!slot) {
            continue;
        }
        if (auto *m = std::get_if<Mutex>(&slot->object); m && m->owner == t.id) {
            m->owner.reset();
            m->lock_count = 0;
        }
    }
}

std::optional<ThreadId> Kernel::resolve_thread(Handle handle) {
    if (handle == CURRENT_THREAD) {
        return current_;
    }
    auto id = resolve(handle);
    if (!id) {
        return std::nullopt;
    }
    if (auto *t = std::get_if<ThreadObject>(object(*id))) {
        return t->id;
    }
    return std::nullopt;
}

} // namespace runtime
