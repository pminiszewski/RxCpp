 // Copyright (c) Microsoft Open Technologies, Inc. All rights reserved. See License.txt in the project root for license information.

#pragma once
#include "rx-includes.hpp"

#if !defined(CPPRX_RX_OPERATORS_HPP)
#define CPPRX_RX_OPERATORS_HPP

namespace rxcpp
{

    //////////////////////////////////////////////////////////////////////
    // 
    // constructors
    template <class T>
    struct CreatedAutoDetachObserver : public Observer<T>
    {
        std::shared_ptr<Observer<T>> observer;
        SerialDisposable disposable;
        
        virtual ~CreatedAutoDetachObserver() {
            clear();
        }

        virtual void OnNext(const T& element)
        {
            if (observer) {
                RXCPP_UNWIND(disposer, [&](){
                    disposable.Dispose();
                });
                observer->OnNext(element);
                disposer.dismiss();
            }
        }
        virtual void OnCompleted() 
        {
            if (observer) {
                RXCPP_UNWIND(disposer, [&](){
                    disposable.Dispose();
                });
                std::shared_ptr<Observer<T>> final;
                using std::swap;
                swap(final, observer);
                final->OnCompleted();
                disposer.dismiss();
            }
        }
        virtual void OnError(const std::exception_ptr& error) 
        {
            if (observer) {
                RXCPP_UNWIND(disposer, [&](){
                    disposable.Dispose();
                });
                std::shared_ptr<Observer<T>> final;
                using std::swap;
                swap(final, observer);
                final->OnError(error);
                disposer.dismiss();
            }
        }
        void clear() 
        {
            observer = nullptr;
        }
    };

    template <class T>
    std::shared_ptr<CreatedAutoDetachObserver<T>> CreateAutoDetachObserver(
        std::shared_ptr<Observer<T>> observer
        )
    {
        auto p = std::make_shared<CreatedAutoDetachObserver<T>>();
        p->observer = std::move(observer);
        
        return p;
    }

    template <class T, class S>
    class CreatedObservable : public Observable<T>
    {
        S subscribe;

    public:
        CreatedObservable(S subscribe)
            : subscribe(std::move(subscribe))
        {
        }
        virtual Disposable Subscribe(std::shared_ptr<Observer<T>> observer)
        {
            auto autoDetachObserver = CreateAutoDetachObserver(std::move(observer));

            if (CurrentThreadScheduler::IsScheduleRequired()) {
                auto scheduler = std::make_shared<CurrentThreadScheduler>();
                scheduler->Schedule(
                   [=](Scheduler::shared) -> Disposable {
                        try {
                            autoDetachObserver->disposable.Set(subscribe(autoDetachObserver));
                        } catch (...) {
                            autoDetachObserver->OnError(std::current_exception());
                        }   
                        return Disposable::Empty();
                   }
                );
                return autoDetachObserver->disposable;
            }
            try {
                autoDetachObserver->disposable.Set(subscribe(autoDetachObserver));
                return autoDetachObserver->disposable;
            } catch (...) {
                autoDetachObserver->OnError(std::current_exception());
            }   
            return Disposable::Empty();
        }
    };

    template <class T, class S>
    std::shared_ptr<Observable<T>> CreateObservable(S subscribe)
    {
        return std::make_shared<CreatedObservable<T,S>>(std::move(subscribe));
    }

    template <class T>
    struct CreatedObserver : public Observer<T>
    {
        std::function<void(const T&)>   onNext;
        std::function<void()>           onCompleted;
        std::function<void(const std::exception_ptr&)> onError;
        
        virtual ~CreatedObserver() {
            clear();
        }

        virtual void OnNext(const T& element)
        {
            if(onNext)
            {
                onNext(element);
            }
        }
        virtual void OnCompleted() 
        {
            if(onCompleted)
            {
                std::function<void()> final;
                using std::swap;
                swap(final, onCompleted);
                clear();
                final();
            }
        }
        virtual void OnError(const std::exception_ptr& error) 
        {
            if(onError)
            {
                std::function<void(const std::exception_ptr&)> final;
                using std::swap;
                swap(final, onError);
                clear();
                final(error);
            }
        }
        void clear() 
        {
            onNext = nullptr;
            onCompleted = nullptr;
            onError = nullptr;
        }
    };

    template <class T>
    std::shared_ptr<Observer<T>> CreateObserver(
        std::function<void(const T&)> onNext,
        std::function<void()> onCompleted = nullptr,
        std::function<void(const std::exception_ptr&)> onError = nullptr
        )
    {
        auto p = std::make_shared<CreatedObserver<T>>();
        p->onNext = std::move(onNext);
        p->onCompleted = std::move(onCompleted);
        p->onError = std::move(onError);
        
        return p;
    }


    namespace detail
    {
        template<class Derived, class T>
        class Sink : public std::enable_shared_from_this<Derived>
        {
            typedef std::shared_ptr<Observer<T>> SinkObserver;
            mutable std::mutex lock;
            mutable util::maybe<Disposable> cancel;

        protected:
            mutable SinkObserver observer;
            
        public:
            Sink(SinkObserver observerArg, Disposable cancelArg) :
                observer(std::move(observerArg))
            {
                cancel.set(std::move(cancelArg));
                if (!observer)
                {
                    observer = std::make_shared<Observer<T>>();
                }
            }
            
            void Dispose() const
            {
                std::unique_lock<std::mutex> guard(lock);
                observer = std::make_shared<Observer<T>>();
                if (cancel)
                {
                    cancel->Dispose();
                    cancel.reset();
                }
            }
            
            Disposable GetDisposable() const
            {
                // make sure to capture state and not 'this'.
                // usage means that 'this' will usualy be destructed
                // immediately
                auto local = this->shared_from_this();
                return Disposable([local]{
                    local->Dispose();
                });
            }
            
            class _ : public Observer<T>
            {
                std::shared_ptr<Derived> that;
            public:
                _(std::shared_ptr<Derived> that) : that(that)
                {}
                
                virtual void OnNext(const T& t)
                {
                    std::unique_lock<std::mutex> guard(that->lock);
                    that->observer->OnNext(t);
                }
                virtual void OnCompleted()
                {
                    std::unique_lock<std::mutex> guard(that->lock);
                    that->observer->OnCompleted();
                    if (that->cancel)
                    {
                        that->cancel->Dispose();
                        that->cancel.reset();
                    }
                }
                virtual void OnError(const std::exception_ptr& e)
                {
                    std::unique_lock<std::mutex> guard(that->lock);
                    that->observer->OnError(e);
                    if (that->cancel)
                    {
                        that->cancel->Dispose();
                        that->cancel.reset();
                    }
                }
            };
        };
        
        template<class Derived, class T>
        class Producer : public std::enable_shared_from_this<Derived>, public Observable<T>
        {
        public:
            typedef std::function<void(Disposable)> SetSink;
            typedef std::function<Disposable(std::shared_ptr<Derived>, std::shared_ptr<Observer<T>>, Disposable, SetSink)> Run;
        private:
            Run run;
            struct State
            {
                SerialDisposable sink;
                SerialDisposable subscription;
            };
        public:
            Producer(Run run) : 
                run(std::move(run))
            {
            }

            virtual Disposable Subscribe(std::shared_ptr<Observer<T>> observer)
            {
                auto state = std::make_shared<State>();
                auto that = this->shared_from_this();
                if (CurrentThreadScheduler::IsScheduleRequired()) {
                    auto scheduler = std::make_shared<CurrentThreadScheduler>();
                    scheduler->Schedule([=](Scheduler::shared) -> Disposable
                    {
                            state->subscription.Set(
                                run(that, observer, state->subscription, [=](Disposable d)
                                {
                                    state->sink.Set(std::move(d));
                                }));
                            return Disposable::Empty();
                    });
                }
                else
                {
                    state->subscription.Set(
                        run(that, observer, state->subscription, [=](Disposable d)
                        {
                            state->sink.Set(std::move(d));
                        }));
                }
                return Disposable([=]()
                {
                    state->sink.Dispose();
                    state->subscription.Dispose();
                });
            }
        };
        
    }

    
    struct SubjectState {
        enum type {
            Invalid,
            Forwarding,
            Completed,
            Error
        };
    };

    template <class T, class Base, class Subject>
    class ObservableSubject : 
        public Base,
        public std::enable_shared_from_this<Subject>
    {
    protected:
        std::mutex lock;
        SubjectState::type state;
        std::exception_ptr error;
        std::vector<std::shared_ptr<Observer<T>>> observers;
        
        virtual ~ObservableSubject() {
            // putting this first means that the observers
            // will be destructed outside the lock
            std::vector<std::shared_ptr<Observer<T>>> empty;

            std::unique_lock<decltype(lock)> guard(lock);
            using std::swap;
            swap(observers, empty);
        }

        ObservableSubject() : state(SubjectState::Forwarding) {
        }

        void RemoveObserver(std::shared_ptr<Observer<T>> toRemove)
        {
            std::unique_lock<decltype(lock)> guard(lock);
            auto it = std::find(begin(observers), end(observers), toRemove);
            if (it != end(observers))
                *it = nullptr;
        }
    public:

        virtual Disposable Subscribe(std::shared_ptr<Observer<T>> observer)
        {
            std::weak_ptr<Observer<T>> wptr = observer;
            std::weak_ptr<Subject> wself = this->shared_from_this();

            Disposable d([wptr, wself]{
                if (auto self = wself.lock())
                {
                    self->RemoveObserver(wptr.lock());
                }
            });

            {
                std::unique_lock<decltype(lock)> guard(lock);
                if (state == SubjectState::Completed) {
                    observer->OnCompleted();
                    return Disposable::Empty();
                } else if (state == SubjectState::Error) {
                    observer->OnError(error);
                    return Disposable::Empty();
                } else {
                    for(auto& o : observers)
                    {
                        if (!o){
                            o = std::move(observer);
                            return d;
                        }
                    }
                    observers.push_back(std::move(observer));
                    return d;
                }
            }
        }
    };

    template <class T, class Base>
    class ObserverSubject : 
        public Observer<T>,
        public Base
    {
    public:
        ObserverSubject() {}

        template<class A>
        explicit ObserverSubject(A&& a) : Base(std::forward<A>(a)) {}

        virtual void OnNext(const T& element)
        {
            std::unique_lock<decltype(Base::lock)> guard(Base::lock);
            auto local = Base::observers;
            guard.unlock();
            for(auto& o : local)
            {
                if (o) {
                    o->OnNext(element);
                }
            }
        }
        virtual void OnCompleted() 
        {
            std::unique_lock<decltype(Base::lock)> guard(Base::lock);
            Base::state = SubjectState::Completed;
            auto local = std::move(Base::observers);
            guard.unlock();
            for(auto& o : local)
            {
                if (o) {
                    o->OnCompleted();
                }
            }
        }
        virtual void OnError(const std::exception_ptr& error) 
        {
            std::unique_lock<decltype(Base::lock)> guard(Base::lock);
            Base::state = SubjectState::Error;
            Base::error = error;
            auto local = std::move(Base::observers);
            guard.unlock();
            for(auto& o : local)
            {
                if (o) {
                    o->OnError(error);
                }
            }
        }
    };

    template <class T>
    class Subject : 
        public ObserverSubject<T, ObservableSubject<T, Observable<T>, Subject<T>>>
    {
    };

    template <class T>
    std::shared_ptr<Subject<T>> CreateSubject()
    {
        return std::make_shared<Subject<T>>();
    }

    template <class K, class T, class Base>
    class GroupedObservableSubject : 
        public Base
    {
        K key;
    public:
        GroupedObservableSubject(K key) : key(std::move(key)) {}

        virtual K Key() {return key;}
    };

    template <class K, class T>
    class GroupedSubject : 
        public ObserverSubject<T, GroupedObservableSubject<K, T, ObservableSubject<T, GroupedObservable<K, T>, GroupedSubject<K, T>>>>
    {
        typedef ObserverSubject<T, GroupedObservableSubject<K, T, ObservableSubject<T, GroupedObservable<K, T>, GroupedSubject<K, T>>>> base;
    public:
        GroupedSubject(K key) : base(std::move(key)) {}
    };

    template <class T, class K>
    std::shared_ptr<GroupedSubject<K, T>> CreateGroupedSubject(K key)
    {
        return std::make_shared<GroupedSubject<K, T>>(std::move(key));
    }

    template <class T>
    class BehaviorSubject : 
        public std::enable_shared_from_this<BehaviorSubject<T>>,
        public Observable<T>,
        public Observer<T>
    {
        std::mutex lock;
        size_t slotCount;
        T value;
        SubjectState::type state;
        util::maybe<std::exception_ptr> error;
        std::vector<std::shared_ptr<Observer<T>>> observers;

        void RemoveObserver(std::shared_ptr<Observer<T>> toRemove)
        {
            std::unique_lock<decltype(lock)> guard(lock);
            auto it = std::find(begin(observers), end(observers), toRemove);
            if (it != end(observers))
            {
                *it = nullptr;
                ++slotCount;
            }
        }

        BehaviorSubject();
    public:

        typedef std::shared_ptr<BehaviorSubject<T>> shared;

        explicit BehaviorSubject(T t) : slotCount(0), value(std::move(t)), state(SubjectState::Forwarding) {}
        
        virtual ~BehaviorSubject() {
            // putting this first means that the observers
            // will be destructed outside the lock
            std::vector<std::shared_ptr<Observer<T>>> empty;

            std::unique_lock<decltype(lock)> guard(lock);
            using std::swap;
            swap(observers, empty);
        }

        virtual Disposable Subscribe(std::shared_ptr<Observer<T>> observer)
        {
            std::weak_ptr<Observer<T>> wptr = observer;
            std::weak_ptr<BehaviorSubject> wself = this->shared_from_this();

            Disposable d([wptr, wself]{
                if (auto self = wself.lock())
                {
                    self->RemoveObserver(wptr.lock());
                }
            });

            SubjectState::type localState = SubjectState::Invalid;
            util::maybe<T> localValue;
            util::maybe<std::exception_ptr> localError;
            {
                std::unique_lock<decltype(lock)> guard(lock);

                localState = state;

                if (state == SubjectState::Forwarding || localState == SubjectState::Completed) 
                {
                    localValue.set(value);
                }
                else if (localState == SubjectState::Error)
                {
                    localError = error;
                }

                if (state == SubjectState::Forwarding)
                {
                    if (slotCount > 0)
                    {
                        for(auto& o : observers)
                        {
                            if (!o)
                            {
                                o = observer;
                                --slotCount;
                                break;
                            }
                        }
                    }
                    else
                    {
                        observers.push_back(observer);
                    }
                }
            }

            if (localState == SubjectState::Completed) {
                observer->OnNext(*localValue.get());
                observer->OnCompleted();
                return Disposable::Empty();
            }
            else if (localState == SubjectState::Error) {
                observer->OnError(*localError.get());
                return Disposable::Empty();
            }
            else if (localState == SubjectState::Forwarding) {
                observer->OnNext(*localValue.get());
            }

            return d;
        }

        virtual void OnNext(const T& element)
        {
            std::unique_lock<decltype(lock)> guard(lock);
            auto local = observers;
            value = element;
            guard.unlock();
            for(auto& o : local)
            {
                if (o) {
                    o->OnNext(element);
                }
            }
        }
        virtual void OnCompleted() 
        {
            std::unique_lock<decltype(lock)> guard(lock);
            state = SubjectState::Completed;
            auto local = std::move(observers);
            guard.unlock();
            for(auto& o : local)
            {
                if (o) {
                    o->OnCompleted();
                }
            }
        }
        virtual void OnError(const std::exception_ptr& errorArg) 
        {
            std::unique_lock<decltype(lock)> guard(lock);
            state = SubjectState::Error;
            error.set(errorArg);
            auto local = std::move(observers);
            guard.unlock();
            for(auto& o : local)
            {
                if (o) {
                    o->OnError(errorArg);
                }
            }
        }
    };

    template <class T, class Arg>
    std::shared_ptr<BehaviorSubject<T>> CreateBehaviorSubject(Arg a)
    {
        return std::make_shared<BehaviorSubject<T>>(std::move(a));
    }

    template <class T>
    class AsyncSubject :
        public std::enable_shared_from_this<AsyncSubject<T>>,
        public Observable<T>,
        public Observer<T>
    {
        std::mutex lock;
        size_t slotCount;
        util::maybe<T> value;
        SubjectState::type state;
        util::maybe<std::exception_ptr> error;
        std::vector < std::shared_ptr < Observer<T >> > observers;

        void RemoveObserver(std::shared_ptr < Observer < T >> toRemove)
        {
            std::unique_lock<decltype(lock)> guard(lock);
            auto it = std::find(begin(observers), end(observers), toRemove);
            if (it != end(observers))
            {
                *it = nullptr;
                ++slotCount;
            }
        }

    public:

        typedef std::shared_ptr<AsyncSubject<T>> shared;

        AsyncSubject() : slotCount(0), value(), state(SubjectState::Forwarding) {}

        virtual ~AsyncSubject() {
            // putting this first means that the observers
            // will be destructed outside the lock
            std::vector < std::shared_ptr < Observer<T >> > empty;

            std::unique_lock<decltype(lock)> guard(lock);
            using std::swap;
            swap(observers, empty);
        }

        virtual Disposable Subscribe(std::shared_ptr < Observer < T >> observer)
        {
            std::weak_ptr<Observer<T>> wptr = observer;
            std::weak_ptr<AsyncSubject> wself = this->shared_from_this();

            Disposable d([wptr, wself]{
                if (auto self = wself.lock())
                {
                    self->RemoveObserver(wptr.lock());
                }
            });

            SubjectState::type localState = SubjectState::Invalid;
            util::maybe<T> localValue;
            util::maybe<std::exception_ptr> localError;
            {
                std::unique_lock<decltype(lock)> guard(lock);

                localState = state;

                if (localState == SubjectState::Completed) 
                {
                    localValue = value;
                }
                else if (localState == SubjectState::Error)
                {
                    localError = error;
                }
                else if (state == SubjectState::Forwarding)
                {
                    if (slotCount > 0)
                    {
                        for (auto& o : observers)
                        {
                            if (!o)
                            {
                                o = observer;
                                --slotCount;
                                break;
                            }
                        }
                    }
                    else
                    {
                        observers.push_back(observer);
                    }
                }
            }

            if (localState == SubjectState::Completed) {
                if (localValue) {
                    observer->OnNext(*localValue.get());
                }
                observer->OnCompleted();
                return Disposable::Empty();
            }
            else if (localState == SubjectState::Error) {
                observer->OnError(*localError.get());
                return Disposable::Empty();
            }

            return d;
        }

        virtual void OnNext(const T& element)
        {
            std::unique_lock<decltype(lock)> guard(lock);
            if (state == SubjectState::Forwarding) {
                value = element;
            }
        }
        virtual void OnCompleted()
        {
            std::unique_lock<decltype(lock)> guard(lock);
            state = SubjectState::Completed;
            auto local = std::move(observers);
            auto localValue = value;
            guard.unlock();
            for (auto& o : local)
            {
                if (o) {
                    if (localValue) {
                        o->OnNext(*localValue.get());
                    }
                    o->OnCompleted();
                }
            }
        }
        virtual void OnError(const std::exception_ptr& errorArg)
        {
            std::unique_lock<decltype(lock)> guard(lock);
            state = SubjectState::Error;
            error.set(errorArg);
            auto local = std::move(observers);
            guard.unlock();
            for (auto& o : local)
            {
                if (o) {
                    o->OnError(errorArg);
                }
            }
        }
    };

    template <class T>
    std::shared_ptr<AsyncSubject<T>> CreateAsyncSubject()
    {
        return std::make_shared<AsyncSubject<T>>();
    }

#if RXCPP_USE_VARIADIC_TEMPLATES
    template<class... A, class F>
    auto ToAsync(F f, Scheduler::shared scheduler = nullptr)
        ->std::function < std::shared_ptr < Observable< decltype(f((*(A*)nullptr)...)) >> (const A&...)>
    {
        typedef decltype(f((*(A*) nullptr)...)) R;
        if (!scheduler)
        {
            scheduler = std::make_shared<EventLoopScheduler>();
        }
        return [=](const A&... a) -> std::shared_ptr < Observable<R >>
        {
            auto args = std::make_tuple(a...);
            auto result = CreateAsyncSubject<R>();
            scheduler->Schedule([=](Scheduler::shared) -> Disposable
            {
                util::maybe<R> value;
                try
                {
                    value.set(util::tuple_dispatch(f, args));
                }
                catch (...)
                {
                    result->OnError(std::current_exception());
                    return Disposable::Empty();
                }
                result->OnNext(*value.get());
                result->OnCompleted();
                return Disposable::Empty();
            });
            return result;
        };
    }
#endif

    template <class Source, class Subject>
    class ConnectableSubject : 
            public std::enable_shared_from_this<ConnectableSubject<Source, Subject>>,
            public ConnectableObservable<typename subject_item<Subject>::type>
    {
    private:
        ConnectableSubject();

        Source source;
        Subject subject;
        util::maybe<Disposable> subscription;
        std::mutex lock;

    public:
        virtual ~ConnectableSubject() {}

        ConnectableSubject(Source source, Subject subject) : source(source), subject(subject)
        {
        }

        virtual Disposable Connect()
        {
            std::unique_lock<std::mutex> guard(lock);
            if (!subscription)
            {
                subscription.set(source->Subscribe(observer(subject)));
            }
            auto that = this->shared_from_this();
            return Disposable([that]()
            {
                std::unique_lock<std::mutex> guard(that->lock);
                if (that->subscription)
                {
                    that->subscription->Dispose();
                    that->subscription.reset();
                }
            });
        }

        virtual Disposable Subscribe(std::shared_ptr < Observer < typename subject_item<Subject>::type >> observer)
        {
            return subject->Subscribe(observer);
        }
    };

    template <class F>
    struct fix0_thunk {
        F f;
        fix0_thunk(F&& f) : f(std::move(f))
        {
        }
        Disposable operator()(Scheduler::shared s) const 
        {
            return f(s, *this);
        }
    };
    template <class F>
    fix0_thunk<F> fix0(F f)
    {
        return fix0_thunk<F>(std::move(f));
    }

    //////////////////////////////////////////////////////////////////////
    // 
    // imperative functions

    template <class S>
    Disposable Subscribe(
        const S& source,
        typename util::identity<std::function<void(const typename observable_item<S>::type&)>>::type onNext,
        std::function<void()> onCompleted = nullptr,
        std::function<void(const std::exception_ptr&)> onError = nullptr
        )
    {
        auto observer = CreateObserver<typename observable_item<S>::type>(
            std::move(onNext), std::move(onCompleted), std::move(onError));
        
        return source->Subscribe(observer);
    }

    template <class T>
    void ForEach(
        const std::shared_ptr<Observable<T>>& source,
        typename util::identity<std::function<void(const T&)>>::type onNext
        )
    {
        std::mutex lock;
        std::condition_variable wake;
        bool done = false;
        std::exception_ptr error;
        auto observer = CreateObserver<T>(std::move(onNext), 
        //on completed
            [&]{
                std::unique_lock<std::mutex> guard(lock);
                done = true;
                wake.notify_one();
            }, 
        //on error
            [&](const std::exception_ptr& e){
                std::unique_lock<std::mutex> guard(lock);
                done = true;
                error = std::move(e);
                wake.notify_one();
            });
        
        source->Subscribe(observer);

        {
            std::unique_lock<std::mutex> guard(lock);
            wake.wait(guard, [&]{return done;});
        }

        if (error != std::exception_ptr()) {
            std::rethrow_exception(error);}
    }
}

#include "operators/Empty.hpp"
#include "operators/Return.hpp"
#include "operators/Throw.hpp"
#include "operators/Range.hpp"
#include "operators/Random.hpp"
#include "operators/Interval.hpp"
#include "operators/Iterate.hpp"
#include "operators/Using.hpp"

//////////////////////////////////////////////////////////////////////
// 
// standard query operators
#include "operators/Select.hpp"
#include "operators/SelectMany.hpp"
#include "operators/Concat.hpp"
#include "operators/CombineLatest.hpp"
#include "operators/Zip.hpp"

namespace rxcpp
{

    namespace detail{
        template<size_t Index, size_t SourcesSize, class SubscribeState>
        struct MergeSubscriber {
            typedef typename SubscribeState::result_type Item;
            typedef std::shared_ptr<Observer<typename SubscribeState::result_type>> ResultObserver;
            static void subscribe(
                ComposableDisposable& cd, 
                const std::shared_ptr<Observer<typename SubscribeState::result_type>>& observer, 
                const std::shared_ptr<SubscribeState>& state,
                const typename SubscribeState::Sources& sources) {
                cd.Add(Subscribe(
                    std::get<Index>(sources),
                // on next
                    [=](const Item& element)
                    {
                        observer->OnNext(element);
                    },
                // on completed
                    [=]
                    {
                        if (--state->pendingComplete == 0) {
                            observer->OnCompleted();
                            cd.Dispose();
                        }
                    },
                // on error
                    [=](const std::exception_ptr& error)
                    {
                        observer->OnError(error);
                        cd.Dispose();
                    }));
                MergeSubscriber<Index + 1, SourcesSize, SubscribeState>::
                    subscribe(cd, observer, state, sources);
            }
        };
        template<size_t SourcesSize, class SubscribeState>
        struct MergeSubscriber<SourcesSize, SourcesSize, SubscribeState> {
            static void subscribe(
                ComposableDisposable& , 
                const std::shared_ptr<Observer<typename SubscribeState::result_type>>& , 
                const std::shared_ptr<SubscribeState>& ,
                const typename SubscribeState::Sources& ) {}
        };
    }
#if RXCPP_USE_VARIADIC_TEMPLATES
    template <class MergeSource, class... MergeSourceNext>
    std::shared_ptr<Observable<MergeSource>> Merge(
        const std::shared_ptr<Observable<MergeSource>>& firstSource,
        const std::shared_ptr<Observable<MergeSourceNext>>&... otherSource
        )
    {
        typedef MergeSource result_type;
        typedef decltype(std::make_tuple(firstSource, otherSource...)) Sources;
        struct State {
            typedef Sources Sources;
            typedef result_type result_type;
            typedef std::tuple_size<Sources> SourcesSize;
            State()
                : pendingComplete(SourcesSize::value)
            {}
            std::atomic<size_t> pendingComplete;
        };
        Sources sources(firstSource, otherSource...);
        // bug on osx prevents using make_shared
        std::shared_ptr<State> state(new State());
        return CreateObservable<result_type>(
            [=](std::shared_ptr<Observer<result_type>> observer) -> Disposable
            {
                ComposableDisposable cd;
                detail::MergeSubscriber<0, State::SourcesSize::value, State>::subscribe(cd, observer, state, sources);
                return cd;
            });
    }
#else
    template <class MergeSource, class MergeSourceNext>
    std::shared_ptr<Observable<MergeSource>> Merge(
        const std::shared_ptr<Observable<MergeSource>>& firstSource,
        const std::shared_ptr<Observable<MergeSourceNext>>& otherSource
        )
    {
        typedef MergeSource result_type;
        typedef decltype(std::make_tuple(firstSource, otherSource)) Sources;
        struct State {
            typedef Sources Sources;
            typedef result_type result_type;
            typedef std::tuple_size<Sources> SourcesSize;
            State()
                : pendingComplete(SourcesSize::value)
            {}
            std::atomic<size_t> pendingComplete;
        };
        Sources sources(firstSource, otherSource);
        // bug on osx prevents using make_shared
        std::shared_ptr<State> state(new State());
        return CreateObservable<result_type>(
            [=](std::shared_ptr<Observer<result_type>> observer) -> Disposable
            {
                ComposableDisposable cd;
                detail::MergeSubscriber<0, State::SourcesSize::value, State>::subscribe(cd, observer, state, sources);
                return cd;
            });
    }
#endif //RXCPP_USE_VARIADIC_TEMPLATES

    template <class T, class P>
    const std::shared_ptr<Observable<T>> Where(
        const std::shared_ptr<Observable<T>>& source,
        P predicate
        )    
    {
        return CreateObservable<T>(
            [=](std::shared_ptr<Observer<T>> observer)
            {
                return Subscribe(
                    source,
                // on next
                    [=](const T& element)
                    {
                        typedef decltype(predicate(element)) U;
                        util::maybe<U> result;
                        try {
                            result.set(predicate(element));
                        } catch(...) {
                            observer->OnError(std::current_exception());
                        }
                        if (!!result && *result.get())
                        {
                            observer->OnNext(element);
                        }
                    },
                // on completed
                    [=]
                    {
                        observer->OnCompleted();
                    },
                // on error
                    [=](const std::exception_ptr& error)
                    {
                        observer->OnError(error);
                    });
            });
    }

    template <class T, class KS, class VS, class L>
    auto GroupBy(
        const std::shared_ptr<Observable<T>>& source,
        KS keySelector,
        VS valueSelector,
        L less) 
        -> std::shared_ptr<Observable<std::shared_ptr<GroupedObservable<
            typename std::decay<decltype(keySelector((*(T*)0)))>::type, 
            typename std::decay<decltype(valueSelector((*(T*)0)))>::type>>>>
    {
        typedef typename std::decay<decltype(keySelector((*(T*)0)))>::type Key;
        typedef typename std::decay<decltype(valueSelector((*(T*)0)))>::type Value;

        typedef std::shared_ptr<GroupedObservable<Key, Value>> LocalGroupObservable;

        return CreateObservable<LocalGroupObservable>(
            [=](std::shared_ptr<Observer<LocalGroupObservable>> observer) -> Disposable
            {
                typedef std::map<Key, std::shared_ptr<GroupedSubject<Key, Value>>, L> Groups;

                struct State
                {
                    explicit State(L less) : groups(std::move(less)) {}
                    std::mutex lock;
                    Groups groups;
                };
                auto state = std::make_shared<State>(std::move(less));

                return Subscribe(
                    source,
                // on next
                    [=](const T& element)
                    {
                        util::maybe<Key> key;
                        try {
                            key.set(keySelector(element));
                        } catch(...) {
                            observer->OnError(std::current_exception());
                        }

                        if (!!key) {
                            auto keySubject = CreateGroupedSubject<Value>(*key.get());

                            typename Groups::iterator groupIt;
                            bool newGroup = false;

                            {
                                std::unique_lock<std::mutex> guard(state->lock);
                                std::tie(groupIt, newGroup) = state->groups.insert(
                                    std::make_pair(*key.get(), keySubject)
                                );
                            }

                            if (newGroup)
                            {
                                LocalGroupObservable nextGroup(std::move(keySubject));
                                observer->OnNext(nextGroup);
                            }

                            util::maybe<Value> result;
                            try {
                                result.set(valueSelector(element));
                            } catch(...) {
                                observer->OnError(std::current_exception());
                            }
                            if (!!result) {
                                groupIt->second->OnNext(std::move(*result.get()));
                            }
                        }
                    },
                // on completed
                    [=]
                    {
                        for(auto& group : state->groups) {
                            group.second->OnCompleted();
                        }
                        observer->OnCompleted();
                    },
                // on error
                    [=](const std::exception_ptr& error)
                    {
                        for(auto& group : state->groups) {
                            group.second->OnError(error);
                        }
                        observer->OnError(error);
                    });
            });
    }


    template <class T, class MulticastSubject>
    std::shared_ptr<ConnectableObservable<T>> Multicast(const std::shared_ptr < Observable < T >> &source, const std::shared_ptr<MulticastSubject>& multicastSubject)
    {
        return std::static_pointer_cast<ConnectableObservable<T>>(
            std::make_shared < ConnectableSubject < std::shared_ptr < Observable < T >> , std::shared_ptr<MulticastSubject> >> (source, multicastSubject));
    }

    template <class T>
    std::shared_ptr<ConnectableObservable<T>> Publish(const std::shared_ptr < Observable < T >> &source)
    {
        auto multicastSubject = std::make_shared<Subject<T>>();
        return Multicast(source, multicastSubject);
    }

    template <class T, class V>
    std::shared_ptr<ConnectableObservable<T>> Publish(const std::shared_ptr < Observable < T >> &source, V value)
    {
        auto multicastSubject = std::make_shared<BehaviorSubject<T>>(value);
        return Multicast(source, multicastSubject);
    }

    template <class T>
    std::shared_ptr<ConnectableObservable<T>> PublishLast(const std::shared_ptr < Observable < T >> &source)
    {
        auto multicastSubject = std::make_shared<AsyncSubject<T>>();
        return Multicast(source, multicastSubject);
    }

    namespace detail
    {
        template<class T>
        class RefCountObservable : public Producer<RefCountObservable<T>, T>
        {
            std::shared_ptr<ConnectableObservable<T>> source;
            std::mutex lock;
            size_t refcount;
            util::maybe<Disposable> subscription;
            
            class _ : public Sink<_, T>, public Observer<T>
            {
                std::shared_ptr<RefCountObservable<T>> parent;
                
            public:
                typedef Sink<_, T> SinkBase;

                _(std::shared_ptr<RefCountObservable<T>> parent, std::shared_ptr<Observer<T>> observer, Disposable cancel) :
                    SinkBase(std::move(observer), std::move(cancel)),
                    parent(parent)
                {
                }
                
                Disposable Run()
                {
                    SerialDisposable subscription;
                    subscription.Set(parent->source->Subscribe(this->shared_from_this()));

                    std::unique_lock<std::mutex> guard(parent->lock);
                    if (++parent->refcount == 1)
                    {
                        parent->subscription.set(parent->source->Connect());
                    }

                    auto local = parent;

                    return Disposable([subscription, local]()
                    {
                        subscription.Dispose();
                        std::unique_lock<std::mutex> guard(local->lock);
                        if (--local->refcount == 0)
                        {
                            local->subscription->Dispose();
                            local->subscription.reset();
                        }
                    });
                }
                
                virtual void OnNext(const T& t)
                {
                    SinkBase::observer->OnNext(t);
                }
                virtual void OnCompleted()
                {
                    SinkBase::observer->OnCompleted();
                    SinkBase::Dispose();
                }
                virtual void OnError(const std::exception_ptr& e)
                {
                    SinkBase::observer->OnError(e);
                    SinkBase::Dispose();
                }
            };
            
            typedef Producer<RefCountObservable<T>, T> ProducerBase;
        public:
            
            RefCountObservable(std::shared_ptr<ConnectableObservable<T>> source) :
                ProducerBase([](std::shared_ptr<RefCountObservable<T>> that, std::shared_ptr<Observer<T>> observer, Disposable&& cancel, typename ProducerBase::SetSink setSink) -> Disposable
                {
                    auto sink = std::shared_ptr<_>(new _(that, observer, std::move(cancel)));
                    setSink(sink->GetDisposable());
                    return sink->Run();
                }),
                refcount(0),
                source(std::move(source))
            {
                subscription.set(Disposable::Empty());
            }
        };
    }
    template <class T>
    const std::shared_ptr<Observable<T>> RefCount(
        const std::shared_ptr<ConnectableObservable<T>>& source
        )
    {
        return std::make_shared<detail::RefCountObservable<T>>(source);
    }

    template <class T>
    const std::shared_ptr<Observable<T>> ConnectForever(
        const std::shared_ptr<ConnectableObservable<T>>& source
        )
    {
        source->Connect();
        return observable(source);
    }

    namespace detail
    {
        template<class T, class A>
        class ScanObservable : public Producer<ScanObservable<T, A>, A>
        {
            typedef ScanObservable<T, A> This;
            typedef std::shared_ptr<This> Parent;
            typedef std::shared_ptr<Observable<T>> Source;
            typedef std::shared_ptr<Observer<A>> Destination;

        public:
            typedef std::function<A(A, T)> Accumulator;
            typedef std::function<util::maybe<A>(T)> Seeder;

        private:

            Source source;
            util::maybe<A> seed;
            Accumulator accumulator;
            Seeder seeder;

            class _ : public Sink<_, A>, public Observer<T>
            {
                Parent parent;
                util::maybe<A> accumulation;

            public:
                typedef Sink<_, A> SinkBase;

                _(Parent parent, Destination observer, Disposable cancel) :
                    SinkBase(std::move(observer), std::move(cancel)),
                    parent(parent)
                {
                }

                virtual void OnNext(const T& t)
                {
                    try
                    {
                        if (accumulation)
                        {
                            accumulation.set(parent->accumulator(*accumulation.get(), t));
                        }
                        else
                        {
                            accumulation.set(!!parent->seed ? parent->accumulator(*parent->seed.get(), t) : *parent->seeder(t).get());
                        }
                    }
                    catch (...)
                    {
                        SinkBase::observer->OnError(std::current_exception());
                        SinkBase::Dispose();
                        return;
                    }
                    SinkBase::observer->OnNext(*accumulation.get());
                }
                virtual void OnCompleted()
                {
                    if (!accumulation && !!parent->seed) {
                        SinkBase::observer->OnNext(*parent->seed.get());
                    }
                    SinkBase::observer->OnCompleted();
                    SinkBase::Dispose();
                }
                virtual void OnError(const std::exception_ptr& e)
                {
                    SinkBase::observer->OnError(e);
                    SinkBase::Dispose();
                }
            };

            typedef Producer<This, A> ProducerBase;
        public:

            ScanObservable(Source source, util::maybe<A> seed, Accumulator accumulator, Seeder seeder) :
                ProducerBase([this](Parent parent, std::shared_ptr < Observer < A >> observer, Disposable && cancel, typename ProducerBase::SetSink setSink) -> Disposable
                {
                    auto sink = std::shared_ptr<_>(new _(parent, observer, std::move(cancel)));
                    setSink(sink->GetDisposable());
                    return this->source->Subscribe(sink);
                }),
                source(std::move(source)),
                seed(std::move(seed)),
                accumulator(std::move(accumulator)),
                seeder(std::move(seeder))
            {
            }
        };
    }
    template <class T, class A>
    std::shared_ptr<Observable<A>> Scan(
        const std::shared_ptr<Observable<T>>& source,
        A seed,
        typename detail::ScanObservable<T, A>::Accumulator accumulator
        )
    {
        return std::make_shared<detail::ScanObservable<T, A>>(
            std::move(source), 
            util::maybe<A>(std::move(seed)), 
            std::move(accumulator),
            [](T) -> util::maybe<A> {abort(); return util::maybe<A>();});
    }
    template <class T>
    std::shared_ptr<Observable<T>> Scan(
        const std::shared_ptr<Observable<T>>& source,
        typename detail::ScanObservable<T, T>::Accumulator accumulator
        )
    {
        return std::make_shared<detail::ScanObservable<T, T>>(
            std::move(source), 
            util::maybe<T>(), 
            std::move(accumulator),
            [](T t) -> util::maybe<T> {return util::maybe<T>(t);});
    }

    template <class T, class Integral>
    std::shared_ptr<Observable<T>> Take(
        const std::shared_ptr<Observable<T>>& source,
        Integral n 
        )    
    {
        return CreateObservable<T>(
            [=](std::shared_ptr<Observer<T>> observer)
            -> Disposable
            {
                // keep count of remaining calls received OnNext and count of OnNext calls issued.
                auto remaining = std::make_shared<std::tuple<std::atomic<Integral>, std::atomic<Integral>>>(n, n);

                ComposableDisposable cd;

                cd.Add(Subscribe(
                    source,
                // on next
                    [=](const T& element)
                    {
                        auto local = --std::get<0>(*remaining);
                        RXCPP_UNWIND_AUTO([&](){
                            if (local >= 0){
                                // all elements received
                                if (--std::get<1>(*remaining) == 0) {
                                    // all elements passed on to observer.
                                    observer->OnCompleted();
                                    cd.Dispose();}}});

                        if (local >= 0) {
                            observer->OnNext(element);
                        } 
                    },
                // on completed
                    [=]
                    {
                        if (std::get<1>(*remaining) == 0 && std::get<0>(*remaining) <= 0) {
                            observer->OnCompleted();
                            cd.Dispose();
                        }
                    },
                // on error
                    [=](const std::exception_ptr& error)
                    {
                        observer->OnError(error);
                        cd.Dispose();
                    }));
                return cd;
            });
    }

    template <class T, class U>
    std::shared_ptr<Observable<T>> TakeUntil(
        const std::shared_ptr<Observable<T>>& source,
        const std::shared_ptr<Observable<U>>& terminus
        )    
    {
        return CreateObservable<T>(
            [=](std::shared_ptr<Observer<T>> observer)
            -> Disposable
            {

                struct TerminusState {
                    enum type {
                        Live,
                        Terminated
                    };
                };
                struct TakeState {
                    enum type {
                        Taking,
                        Completed
                    };
                };
                struct State {
                    State() : terminusState(TerminusState::Live), takeState(TakeState::Taking) {}
                    std::atomic<typename TerminusState::type> terminusState;
                    std::atomic<typename TakeState::type> takeState;
                };
                auto state = std::make_shared<State>();

                ComposableDisposable cd;

                cd.Add(Subscribe(
                    terminus,
                // on next
                    [=](const T& element)
                    {
                        state->terminusState = TerminusState::Terminated;
                    },
                // on completed
                    [=]
                    {
                        state->terminusState = TerminusState::Terminated;
                    },
                // on error
                    [=](const std::exception_ptr& error)
                    {
                        state->terminusState = TerminusState::Terminated;
                    }));

                cd.Add(Subscribe(
                    source,
                // on next
                    [=](const T& element)
                    {
                        if (state->terminusState == TerminusState::Live) {
                            observer->OnNext(element);
                        } else if (state->takeState.exchange(TakeState::Completed) == TakeState::Taking) {
                            observer->OnCompleted();
                            cd.Dispose();
                        }
                    },
                // on completed
                    [=]
                    {
                        if (state->takeState.exchange(TakeState::Completed) == TakeState::Taking) {
                            state->terminusState = TerminusState::Terminated;
                            observer->OnCompleted();
                            cd.Dispose();
                        }
                    },
                // on error
                    [=](const std::exception_ptr& error)
                    {
                        state->takeState = TakeState::Completed;
                        state->terminusState = TerminusState::Terminated;
                        observer->OnError(error);
                        cd.Dispose();
                    }));
                return cd;
            });
    }

    template <class T, class Integral>
    std::shared_ptr<Observable<T>> Skip(
        const std::shared_ptr<Observable<T>>& source,
        Integral n
        )    
    {
        return CreateObservable<T>(
            [=](std::shared_ptr<Observer<T>> observer)
            -> Disposable
            {
                struct State {
                    enum type {
                        Skipping,
                        Forwarding
                    };
                };
                // keep count of remaining OnNext calls to skip and state.
                auto remaining = std::make_shared<std::tuple<std::atomic<Integral>, std::atomic<typename State::type>>>(n, State::Skipping);

                ComposableDisposable cd;

                cd.Add(Subscribe(
                    source,
                // on next
                    [=](const T& element)
                    {
                        if (std::get<1>(*remaining) == State::Forwarding) {
                            observer->OnNext(element);
                        } else {
                            auto local = --std::get<0>(*remaining);

                            if (local == 0) {
                                std::get<1>(*remaining) = State::Forwarding;
                            }
                        }
                    },
                // on completed
                    [=]
                    {
                        observer->OnCompleted();
                        cd.Dispose();
                    },
                // on error
                    [=](const std::exception_ptr& error)
                    {
                        observer->OnError(error);
                        cd.Dispose();
                    }));
                return cd;
            });
    }

    template <class T, class U>
    std::shared_ptr<Observable<T>> SkipUntil(
        const std::shared_ptr<Observable<T>>& source,
        const std::shared_ptr<Observable<U>>& terminus
        )    
    {
        return CreateObservable<T>(
            [=](std::shared_ptr<Observer<T>> observer)
            -> Disposable
            {
                struct SkipState {
                    enum type {
                        Skipping,
                        Taking
                    };
                };
                struct State {
                    State() : skipState(SkipState::Skipping) {}
                    std::atomic<typename SkipState::type> skipState;
                };
                auto state = std::make_shared<State>();

                ComposableDisposable cd;

                cd.Add(Subscribe(
                    terminus,
                // on next
                    [=](const T& element)
                    {
                        state->skipState = SkipState::Taking;
                    },
                // on completed
                    [=]
                    {
                        state->skipState = SkipState::Taking;
                    },
                // on error
                    [=](const std::exception_ptr& error)
                    {
                        state->skipState = SkipState::Taking;
                    }));

                cd.Add(Subscribe(
                    source,
                // on next
                    [=](const T& element)
                    {
                        if (state->skipState == SkipState::Taking) {
                            observer->OnNext(element);
                        }
                    },
                // on completed
                    [=]
                    {
                        observer->OnCompleted();
                        cd.Dispose();
                    },
                // on error
                    [=](const std::exception_ptr& error)
                    {
                        observer->OnError(error);
                        cd.Dispose();
                    }));
                return cd;
            });
    }


    template <class StdCollection>
    std::shared_ptr<Observable<StdCollection>> ToStdCollection(
        const std::shared_ptr<Observable<typename StdCollection::value_type>>& source
        )
    {
        typedef typename StdCollection::value_type Value;
        return CreateObservable<StdCollection>(
            [=](std::shared_ptr<Observer<StdCollection>> observer) -> Disposable
            {
                auto stdCollection = std::make_shared<StdCollection>();
                return Subscribe(
                    source,
                // on next
                    [=](const Value& element)
                    {
                        stdCollection->insert(stdCollection->end(), element);
                    },
                // on completed
                    [=]
                    {
                        observer->OnNext(std::move(*stdCollection.get()));
                        observer->OnCompleted();
                    },
                // on error
                    [=](const std::exception_ptr& error)
                    {
                        observer->OnError(error);
                    });
            });
    }

    //////////////////////////////////////////////////////////////////////
    // 
    // time

    template <class T>
    std::shared_ptr<Observable<T>> Delay(
        const std::shared_ptr<Observable<T>>& source,
        Scheduler::clock::duration due,
        Scheduler::shared scheduler)
    {
        return CreateObservable<T>(
            [=](std::shared_ptr<Observer<T>> observer)
            -> Disposable
            {
                auto cancel = std::make_shared<bool>(false);

                ComposableDisposable cd;

                cd.Add(Disposable([=]{ 
                    *cancel = true; }));

                SerialDisposable sd;
                auto wsd = cd.Add(sd);

                cd.Add(Subscribe(
                    source,
                // on next
                    [=](const T& element)
                    {
                        auto sched_disposable = scheduler->Schedule(
                            due, 
                            [=] (Scheduler::shared) -> Disposable { 
                                if (!*cancel)
                                    observer->OnNext(element); 
                                return Disposable::Empty();
                            }
                        );
                        auto ssd = wsd.lock();
                        if (ssd)
                        {
                            *ssd.get() = std::move(sched_disposable);
                        }
                    },
                // on completed
                    [=]
                    {
                        auto sched_disposable = scheduler->Schedule(
                            due, 
                            [=](Scheduler::shared) -> Disposable { 
                                if (!*cancel)
                                    observer->OnCompleted(); 
                                return Disposable::Empty();
                            }
                        );
                        auto ssd = wsd.lock();
                        if (ssd)
                        {
                            *ssd.get() = std::move(sched_disposable);
                        }
                    },
                // on error
                    [=](const std::exception_ptr& error)
                    {
                        if (!*cancel)
                            observer->OnError(error);
                    }));
                return cd;
            });
    }

    template <class T>
    std::shared_ptr<Observable<T>> Throttle(
        const std::shared_ptr<Observable<T>>& source,
        Scheduler::clock::duration due,
        Scheduler::shared scheduler)
    {
        return CreateObservable<T>(
            [=](std::shared_ptr<Observer<T>> observer)
            -> Disposable
            {
                struct State {
                    State() : hasValue(false), id(0) {}
                    std::mutex lock;
                    T value;
                    bool hasValue;
                    size_t id;
                };
                auto state = std::make_shared<State>();

                ComposableDisposable cd;

                SerialDisposable sd;
                cd.Add(sd);

                cd.Add(Subscribe(
                    source,
                // on next
                    [=](const T& element)
                    {
                        size_t current = 0;
                        {
                            std::unique_lock<std::mutex> guard(state->lock);
                            state->hasValue = true;
                            state->value = std::move(element);
                            current = ++state->id;
                        }
                        sd.Set(scheduler->Schedule(
                            due, 
                            [=] (Scheduler::shared) -> Disposable { 
                                {
                                    std::unique_lock<std::mutex> guard(state->lock);
                                    if (state->hasValue && state->id == current) {
                                        observer->OnNext(std::move(state->value));
                                    }
                                    state->hasValue = false;
                                }

                                return Disposable::Empty();
                            }
                        ));
                    },
                // on completed
                    [=]
                    {
                        bool sendValue = false;
                        T value;
                        {
                            std::unique_lock<std::mutex> guard(state->lock);
                            sendValue = state->hasValue;
                            if (sendValue) {
                                value = std::move(state->value);}
                            state->hasValue = false;
                            ++state->id;
                        }
                        if (sendValue) {
                            observer->OnNext(std::move(value));}
                        observer->OnCompleted();
                        cd.Dispose();
                    },
                // on error
                    [=](const std::exception_ptr& error)
                    {
                        {
                            std::unique_lock<std::mutex> guard(state->lock);
                            state->hasValue = false;
                            ++state->id;
                        }
                        observer->OnError(error);
                        cd.Dispose();
                    }));
                return cd;
            });
    }


    // no more than one event ever 'milliseconds'
    // TODO: oops, this is not the right definition for throttle.
    template <class T>
    std::shared_ptr<Observable<T>> LimitWindow(
        const std::shared_ptr<Observable<T>>& source,
        int milliseconds)
    {
        if (milliseconds == 0)
            return source;
        
        return CreateObservable<T>(
            [=](std::shared_ptr<Observer<T>> observer)
            -> Disposable
            {
                struct State {
                    std::chrono::steady_clock::time_point dueTime;
                };
        
                auto state = std::make_shared<State>();

                return Subscribe(
                    source,
                // on next
                    [=](const T& element)
                    {
                        auto now = std::chrono::steady_clock::now();

                        if (now >= state->dueTime)
                        {
                            observer->OnNext(element);
                            state->dueTime = now + std::chrono::duration<int, std::milli>(milliseconds);
                        }
                    },
                // on completed
                    [=]
                    {
                        observer->OnCompleted();
                    },
                // on error
                    [=](const std::exception_ptr& error)
                    {
                        observer->OnError(error);
                    });
            });
    }

    // removes duplicate-sequenced values. e.g. 1,2,2,3,1 ==> 1,2,3,1
    template <class T>
    std::shared_ptr<Observable<T>> DistinctUntilChanged(
        const std::shared_ptr<Observable<T>>& source)
    {   
        return CreateObservable<T>(
            [=](std::shared_ptr<Observer<T>> observer)
            -> Disposable
            {
                struct State {
                    State() : last(), hasValue(false) {}
                    T last; bool hasValue;
                };
        
                auto state = std::make_shared<State>();
                state->hasValue = false;

                return Subscribe(
                    source,
                // on next
                    [=](const T& element)
                    {
                        if (!state->hasValue || !(state->last == element))
                        {
                            observer->OnNext(element);
                            state->last = element;
                            state->hasValue = true;
                        }
                    },
                // on completed
                    [=]
                    {
                        observer->OnCompleted();
                    },
                // on error
                    [=](const std::exception_ptr& error)
                    {
                        observer->OnError(error);
                    });
            });
    }

    template <class T>
    std::shared_ptr<Observable<T>> SubscribeOnObservable(
        const std::shared_ptr<Observable<T>>& source, 
        Scheduler::shared scheduler)
    {
        return CreateObservable<T>(
            [=](std::shared_ptr<Observer<T>> observer)
            -> Disposable
            {
                ComposableDisposable cd;

                SerialDisposable sd;
                cd.Add(sd);

                cd.Add(scheduler->Schedule([=](Scheduler::shared) -> Disposable {
                    sd.Set(ScheduledDisposable(scheduler, source->Subscribe(observer)));
                    return Disposable::Empty();
                }));
                return cd;
            });
    }

    template <class T>
    std::shared_ptr<Observable<T>> ObserveOnObserver(
        const std::shared_ptr<Observable<T>>& source, 
        Scheduler::shared scheduler)
    {
        return CreateObservable<T>(
            [=](std::shared_ptr<Observer<T>> observerArg)
            -> Disposable
            {
                std::shared_ptr<ScheduledObserver<T>> observer(
                    new ScheduledObserver<T>(scheduler, std::move(observerArg)));

                ComposableDisposable cd;

                cd.Add(*observer.get());

                cd.Add(Subscribe(
                    source,
                // on next
                    [=](const T& element)
                    {
                        observer->OnNext(std::move(element));
                        observer->EnsureActive();
                    },
                // on completed
                    [=]
                    {
                        observer->OnCompleted();
                        observer->EnsureActive();
                    },
                // on error
                    [=](const std::exception_ptr& error)
                    {
                        observer->OnError(std::move(error));
                        observer->EnsureActive();
                    }));
                return cd;
            });
    }

    class StdQueueDispatcher
    {
        mutable std::queue<std::function<void()>> pending;
        mutable std::condition_variable wake;
        mutable std::mutex pendingLock;

        std::function<void()> get() const
        {
            std::function<void()> fn;
            fn = std::move(pending.front());
            pending.pop();
            return std::move(fn);
        }

        void dispatch(std::function<void()> fn) const
        {
            if (fn)
            {
                try {
                    fn();
                }
                catch(...) {
                    std::unexpected();
                }
            }
        }

    public:
        template <class Fn>
        void post(Fn fn) const
        {
            {
                std::unique_lock<std::mutex> guard(pendingLock);
                pending.push(std::move(fn));
            }
            wake.notify_one();
        }

        void try_dispatch() const
        {
            std::function<void()> fn;
            {
                std::unique_lock<std::mutex> guard(pendingLock);
                if (!pending.empty())
                {
                    fn = get();
                }
            }
            dispatch(std::move(fn));
        }

        bool dispatch_one() const
        {
            std::function<void()> fn;
            {
                std::unique_lock<std::mutex> guard(pendingLock);
                wake.wait(guard, [this]{ return !pending.empty();});
                fn = get();
            }
            bool result = !!fn;
            dispatch(std::move(fn));
            return result;
        }
    };
#if defined(OBSERVE_ON_DISPATCHER_OP)
    typedef OBSERVE_ON_DISPATCHER_OP ObserveOnDispatcherOp;
#else
    typedef StdQueueDispatcher ObserveOnDispatcherOp;
#endif 

    template <class T>
    std::shared_ptr<Observable<T>> ObserveOnDispatcher(
        const std::shared_ptr<Observable<T>>& source)
    {
        auto dispatcher = std::make_shared<ObserveOnDispatcherOp>();

        return CreateObservable<T>(
            [=](std::shared_ptr<Observer<T>> observer)
            -> Disposable
            {
                auto cancel = std::make_shared<bool>(false);

                ComposableDisposable cd;

                cd.Add(Disposable([=]{ 
                    *cancel = true; 
                }));
                cd.Add(Subscribe(
                    source,
                // on next
                    [=](const T& element)
                    {
                        dispatcher->post([=]{
                            if (!*cancel)
                                observer->OnNext(element); 
                        });
                    },
                // on completed
                    [=]
                    {
                        dispatcher->post([=]{
                            if(!*cancel)
                                observer->OnCompleted(); 
                        });
                    },
                // on error
                    [=](const std::exception_ptr& error)
                    {
                        dispatcher->post([=]{
                            if (!*cancel)
                                observer->OnError(error); 
                        });
                    }));
                return cd;
            });
    }

}

#endif
