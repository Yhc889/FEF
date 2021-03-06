

#ifndef RIPPLE_TEST_CSF_SCHEDULER_H_INCLUDED
#define RIPPLE_TEST_CSF_SCHEDULER_H_INCLUDED

#include <ripple/basics/qalloc.h>
#include <ripple/beast/clock/manual_clock.h>
#include <boost/intrusive/set.hpp>
#include <type_traits>
#include <utility>

namespace ripple {
namespace test {
namespace csf {


class Scheduler
{
public:
    using clock_type = beast::manual_clock<std::chrono::steady_clock>;

    using duration = typename clock_type::duration;

    using time_point = typename clock_type::time_point;

private:
    using by_when_hook = boost::intrusive::set_base_hook<
        boost::intrusive::link_mode<boost::intrusive::normal_link>>;

    struct event : by_when_hook
    {
        time_point when;

        event(event const&) = delete;
        event&
        operator=(event const&) = delete;

        virtual ~event() = default;

        virtual void
        operator()() const = 0;

        event(time_point when_) : when(when_)
        {
        }

        bool
        operator<(event const& other) const
        {
            return when < other.when;
        }
    };

    template <class Handler>
    class event_impl : public event
    {
        Handler const h_;

    public:
        event_impl(event_impl const&) = delete;

        event_impl&
        operator=(event_impl const&) = delete;

        template <class DeducedHandler>
        event_impl(time_point when_, DeducedHandler&& h)
            : event(when_)
            , h_(std::forward<DeducedHandler>(h))
        {
        }

        void
        operator()() const override
        {
            h_();
        }
    };

    class queue_type
    {
    private:
        using by_when_set = typename boost::intrusive::make_multiset<
            event,
            boost::intrusive::constant_time_size<false>>::type;

        qalloc alloc_;
        by_when_set by_when_;

    public:
        using iterator = typename by_when_set::iterator;

        queue_type(queue_type const&) = delete;
        queue_type&
        operator=(queue_type const&) = delete;

        explicit queue_type(qalloc const& alloc);

        ~queue_type();

        bool
        empty() const;

        iterator
        begin();

        iterator
        end();

        template <class Handler>
        typename by_when_set::iterator
        emplace(time_point when, Handler&& h);

        iterator
        erase(iterator iter);
    };

    qalloc alloc_;
    queue_type queue_;

    mutable clock_type clock_;

public:
    Scheduler(Scheduler const&) = delete;
    Scheduler&
    operator=(Scheduler const&) = delete;

    Scheduler();

    
    qalloc const&
    alloc() const;

    
    clock_type &
    clock() const;

    
    time_point
    now() const;

    struct cancel_token;

    
    template <class Function>
    cancel_token
    at(time_point const& when, Function&& f);

    
    template <class Function>
    cancel_token
    in(duration const& delay, Function&& f);

    
    void
    cancel(cancel_token const& token);

    
    bool
    step_one();

    
    bool
    step();

    
    template <class Function>
    bool
    step_while(Function&& func);

    
    bool
    step_until(time_point const& until);

    
    template <class Period, class Rep>
    bool
    step_for(std::chrono::duration<Period, Rep> const& amount);
};


inline Scheduler::queue_type::queue_type(qalloc const& alloc) : alloc_(alloc)
{
}

inline Scheduler::queue_type::~queue_type()
{
    for (auto iter = by_when_.begin(); iter != by_when_.end();)
    {
        auto e = &*iter;
        ++iter;
        e->~event();
        alloc_.dealloc(e, 1);
    }
}

inline bool
Scheduler::queue_type::empty() const
{
    return by_when_.empty();
}

inline auto
Scheduler::queue_type::begin() -> iterator
{
    return by_when_.begin();
}

inline auto
Scheduler::queue_type::end() -> iterator
{
    return by_when_.end();
}


template <class Handler>
inline auto
Scheduler::queue_type::emplace(time_point when, Handler&& h) ->
    typename by_when_set::iterator
{
    using event_type = event_impl<std::decay_t<Handler>>;
    auto const p = alloc_.alloc<event_type>(1);
    auto& e = *new (p) event_type(
        when, std::forward<Handler>(h));
    return by_when_.insert(e);
}

inline auto
Scheduler::queue_type::erase(iterator iter) -> typename by_when_set::iterator
{
    auto& e = *iter;
    auto next = by_when_.erase(iter);
    e.~event();
    alloc_.dealloc(&e, 1);
    return next;
}

struct Scheduler::cancel_token
{
private:
    typename queue_type::iterator iter_;

public:
    cancel_token() = delete;
    cancel_token(cancel_token const&) = default;
    cancel_token&
    operator=(cancel_token const&) = default;

private:
    friend class Scheduler;
    cancel_token(typename queue_type::iterator iter) : iter_(iter)
    {
    }
};

inline Scheduler::Scheduler() : queue_(alloc_)
{
}

inline qalloc const&
Scheduler::alloc() const
{
    return alloc_;
}

inline auto
Scheduler::clock() const -> clock_type &
{
    return clock_;
}

inline auto
Scheduler::now() const -> time_point
{
    return clock_.now();
}

template <class Function>
inline auto
Scheduler::at(time_point const& when, Function&& f) -> cancel_token
{
    return queue_.emplace(when, std::forward<Function>(f));
}

template <class Function>
inline auto
Scheduler::in(duration const& delay, Function&& f) -> cancel_token
{
    return at(clock_.now() + delay, std::forward<Function>(f));
}

inline void
Scheduler::cancel(cancel_token const& token)
{
    queue_.erase(token.iter_);
}

inline bool
Scheduler::step_one()
{
    if (queue_.empty())
        return false;
    auto const iter = queue_.begin();
    clock_.set(iter->when);
    (*iter)();
    queue_.erase(iter);
    return true;
}

inline bool
Scheduler::step()
{
    if (!step_one())
        return false;
    for (;;)
        if (!step_one())
            break;
    return true;
}

template <class Function>
inline bool
Scheduler::step_while(Function&& f)
{
    bool ran = false;
    while (f() && step_one())
        ran = true;
    return ran;
}

inline bool
Scheduler::step_until(time_point const& until)
{
    if (queue_.empty())
    {
        clock_.set(until);
        return false;
    }
    auto iter = queue_.begin();
    if (iter->when > until)
    {
        clock_.set(until);
        return true;
    }
    do
    {
        step_one();
        iter = queue_.begin();
    } while (iter != queue_.end() && iter->when <= until);
    clock_.set(until);
    return iter != queue_.end();
}

template <class Period, class Rep>
inline bool
Scheduler::step_for(std::chrono::duration<Period, Rep> const& amount)
{
    return step_until(now() + amount);
}

}  
}  
}  

#endif








