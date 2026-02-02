//
// JavaScript 风格 Promise：executor 构造、链式 then、resolve/reject
//
#include <exception>
#include <functional>
#include <string>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace CPromise {

enum class PromiseState { Pending, Fulfilled, Rejected };

struct PromiseError {
    std::string message;
    int code{0};
    PromiseError() = default;
    PromiseError(const std::string& msg, int c = 0) : message(msg), code(c) {}

    /// 从 std::exception 构造（用于 try-catch）
    static PromiseError fromException(const std::exception& e) {
        return PromiseError(std::string(e.what()), -1);
    }
    static PromiseError unknownException() {
        return PromiseError("Unknown exception", -1);
    }
};

// 前向声明与 trait：then 链返回类型
template<typename T>
class Promise;

template<typename T>
struct PromiseTraits {
    static constexpr bool isPromise = false;
    using ValueType = T;
};
template<typename U>
struct PromiseTraits<Promise<U>> {
    static constexpr bool isPromise = true;
    using ValueType = U;
};

template<typename T>
class Promise {
public:
    using ValueType = T;
    using OnFulfilled = std::function<void(const T&)>;
    using OnRejected  = std::function<void(const PromiseError&)>;
    using ResolveFn   = std::function<void(T)>;
    using RejectFn   = std::function<void(const PromiseError&)>;
    using Executor   = std::function<void(ResolveFn, RejectFn)>;

    Promise() = default;
    
    /// JavaScript 风格：new Promise((resolve, reject) => { ... })
    explicit Promise(Executor executor) {
        d = std::make_shared<State>();
        // Capture shared_ptr instead of this to ensure thread safety and lifetime safety
        auto statePtr = d;
        m_resolveFn = [statePtr](T value) {
            if (!statePtr || statePtr->state != PromiseState::Pending) return;
            // Handle Promise<Promise<T>> case
            if constexpr (PromiseTraits<T>::isPromise) {
                // Check for cycle
                if (value.d && value.d == statePtr) {
                    statePtr->state = PromiseState::Rejected;
                    statePtr->error = PromiseError("Chaining cycle detected for promise", -1);
                    statePtr->maybeInvoke();
                    return;
                }
                // Wait for nested Promise to complete
                value.then([statePtr](const typename PromiseTraits<T>::ValueType& v) {
                    if (!statePtr || statePtr->state != PromiseState::Pending) return;
                    statePtr->state = PromiseState::Fulfilled;
                    statePtr->value = v;
                    statePtr->maybeInvoke();
                }).catchError([statePtr](const PromiseError& e) {
                    if (!statePtr || statePtr->state != PromiseState::Pending) return;
                    statePtr->state = PromiseState::Rejected;
                    statePtr->error = e;
                    statePtr->maybeInvoke();
                });
            } else {
                statePtr->state = PromiseState::Fulfilled;
                statePtr->value = std::move(value);
                statePtr->maybeInvoke();
            }
        };
        m_rejectFn = [statePtr](const PromiseError& err) {
            if (!statePtr || statePtr->state != PromiseState::Pending) return;
            statePtr->state = PromiseState::Rejected;
            statePtr->error = err;
            statePtr->maybeInvoke();
        };
        try {
            executor(m_resolveFn, m_rejectFn);
        } catch (const std::exception& e) {
            m_rejectFn(PromiseError::fromException(e));
        } catch (...) {
            m_rejectFn(PromiseError::unknownException());
        }
    }

    void resolve(T value) {
        if (m_resolveFn) {
            m_resolveFn(std::move(value));
        }
    }

    void reject(const PromiseError& err) {
        if (m_rejectFn) {
            m_rejectFn(err);
        }
    }

public:

    void reject(const std::string& message, int code = 0) {
        reject(PromiseError(message, code));
    }
    

    //1.std::invoke_result_t<F, const T&>  作用：推断调用 onFulfilled(const T&) 的返回类型 如果 onFulfilled 返回 int，则 R = int
    //如果返回 Promise<int>，则 R = Promise<int>
    //如果返回 void，则 R = void
    //如果返回其他类型，则 R = 其他类型
    //2.PromiseTraits<R>::isPromise
    //作用：判断返回类型 R 是否为 Promise
    //R = int → isPromise = false
    //R = Promise<int> → isPromise = true
    //std::conditional_t<条件, 类型A, 类型B>
    //作用：条件类型选择
    //如果 条件 为 true，选择类型A；否则选择类型B

    template<typename F, typename G = std::nullptr_t>
    auto then(F&& onFulfilled, G&& onRejected) -> Promise<std::conditional_t<
        PromiseTraits<std::invoke_result_t<F, const T&>>::isPromise,
        typename PromiseTraits<std::invoke_result_t<F, const T&>>::ValueType,
        std::decay_t<std::invoke_result_t<F, const T&>>>> {
        using R = std::invoke_result_t<F, const T&>;
        using U = std::conditional_t<
            PromiseTraits<R>::isPromise,
            typename PromiseTraits<R>::ValueType,
            std::decay_t<R>>;
        // executor 中保存 resolve/reject 函数，并根据原 Promise 的状态决定执行方式
        Promise<U> next([onFulfilled = std::forward<F>(onFulfilled), 
                         onRejected = std::forward<G>(onRejected),
                         currentState = d](typename Promise<U>::ResolveFn resolve, typename Promise<U>::RejectFn reject) mutable {
            // 创建处理回调
            auto handleFulfilled = [onFulfilled = std::forward<F>(onFulfilled), resolve, reject, currentState](const T& v) mutable {
                try {
                    if constexpr (std::is_same_v<R, void>) {
                        onFulfilled(v);
                        if constexpr (std::is_same_v<U, void>) {
                            resolve();
                        } else {
                            reject(PromiseError("Invalid return type", -1));
                        }
                    } else {
                        R result = onFulfilled(v);
                        if constexpr (PromiseTraits<R>::isPromise) {
                            // 循环检测：检查返回的 Promise 是否指向当前 Promise 的状态
                            if (result.d && result.d == currentState) {
                                reject(PromiseError("Chaining cycle detected for promise", -1));
                                return;
                            }
                            // Handle Promise<void> case: then callback takes no parameters
                            if constexpr (std::is_same_v<typename PromiseTraits<R>::ValueType, void>) {
                                result.then([resolve]() mutable { resolve(); });
                            } else {
                                result.then([resolve](const U& u) mutable { resolve(u); });
                            }
                            result.catchError([reject](const PromiseError& e) mutable { reject(e); });
                        } else {
                            resolve(std::move(result));
                        }
                    }
                } catch (const std::exception& e) {
                    reject(PromiseError::fromException(e));
                } catch (...) {
                    reject(PromiseError::unknownException());
                }
            };

            auto handleRejected = [onRejected = std::forward<G>(onRejected), resolve, reject, currentState](const PromiseError& e) mutable {
                if constexpr (std::is_same_v<G, std::nullptr_t>) {
                    reject(e);
                } else {
                    try {
                        using R2 = std::invoke_result_t<G, const PromiseError&>;
                        if constexpr (std::is_same_v<R2, void>) {
                            onRejected(e);
                            if constexpr (std::is_same_v<U, void>) {
                                resolve();
                            } else {
                                reject(PromiseError("Invalid return type", -1));
                            }
                        } else {
                            R2 result = onRejected(e);
                            if constexpr (PromiseTraits<R2>::isPromise) {
                                if (result.d && result.d == currentState) {
                                    reject(PromiseError("Chaining cycle detected for promise", -1));
                                    return;
                                }
                                // Handle Promise<void> case: then callback takes no parameters
                                if constexpr (std::is_same_v<typename PromiseTraits<R2>::ValueType, void>) {
                                    result.then([resolve]() mutable { resolve(); });
                                } else {
                                    result.then([resolve](const U& u) mutable { resolve(u); });
                                }
                                result.catchError([reject](const PromiseError& err) mutable { reject(err); });
                            } else {
                                resolve(std::move(result));
                            }
                        }
                    } catch (const std::exception& ex) {
                        reject(PromiseError::fromException(ex));
                    } catch (...) {
                        reject(PromiseError::unknownException());
                    }
                }
            };

            // 根据当前 Promise 的状态决定执行方式
            if (currentState->state == PromiseState::Fulfilled) {
                // 当前 Promise 已完成，立即执行 onFulfilled
                handleFulfilled(currentState->value);
            } else if (currentState->state == PromiseState::Rejected) {
                // 当前 Promise 已失败，执行 onRejected
                handleRejected(currentState->error);
            } else {
                // 当前 Promise 还在 pending，仅注册回调；resolve/reject 时会调用 maybeInvoke
                currentState->thenCallbacks.push_back(std::move(handleFulfilled));
                currentState->catchCallbacks.push_back(std::move(handleRejected));
            }
        });
        
        return next;
    }


    template<typename F>
    auto then(F&& onFulfilled) -> Promise<std::conditional_t<
        PromiseTraits<std::invoke_result_t<F, const T&>>::isPromise,
        typename PromiseTraits<std::invoke_result_t<F, const T&>>::ValueType,
        std::decay_t<std::invoke_result_t<F, const T&>>>> {
        return then(std::forward<F>(onFulfilled), static_cast<std::nullptr_t>(nullptr));
    }

    /// Overload for then(f, nullptr): no onRejected, so never instantiate invoke_result_t<G, ...> with G=nullptr_t
    template<typename F>
    auto then(F&& onFulfilled, std::nullptr_t) -> Promise<std::conditional_t<
        PromiseTraits<std::invoke_result_t<F, const T&>>::isPromise,
        typename PromiseTraits<std::invoke_result_t<F, const T&>>::ValueType,
        std::decay_t<std::invoke_result_t<F, const T&>>>> {
        using R = std::invoke_result_t<F, const T&>;
        using U = std::conditional_t<
            PromiseTraits<R>::isPromise,
            typename PromiseTraits<R>::ValueType,
            std::decay_t<R>>;
        if (!d) {
            return Promise<U>([](typename Promise<U>::ResolveFn, typename Promise<U>::RejectFn reject) {
                reject(PromiseError("Promise has no state", -1));
            });
        }
        Promise<U> next([onFulfilled = std::forward<F>(onFulfilled), currentState = d](
            typename Promise<U>::ResolveFn resolve, typename Promise<U>::RejectFn reject) mutable {
            auto handleFulfilled = [onFulfilled = std::forward<F>(onFulfilled), resolve, reject, currentState](const T& v) mutable {
                try {
                    if constexpr (std::is_same_v<R, void>) {
                        onFulfilled(v);
                        if constexpr (std::is_same_v<U, void>) { resolve(); }
                        else { reject(PromiseError("Invalid return type", -1)); }
                    } else {
                        R result = onFulfilled(v);
                        if constexpr (PromiseTraits<R>::isPromise) {
                            if (result.d && result.d == currentState) {
                                reject(PromiseError("Chaining cycle detected for promise", -1));
                                return;
                            }
                            // Handle Promise<void> case: then callback takes no parameters
                            if constexpr (std::is_same_v<typename PromiseTraits<R>::ValueType, void>) {
                                 result.then([resolve]() mutable { resolve(); });
                             } else {
                                result.then([resolve](const U& u) mutable { resolve(u); });
                             }
                            result.catchError([reject](const PromiseError& e) mutable { reject(e); });
                        } else {
                            resolve(std::move(result));
                        }
                    }
                } catch (const std::exception& e) {
                    reject(PromiseError::fromException(e));
                } catch (...) {
                    reject(PromiseError::unknownException());
                }
            };
            auto handleRejected = [reject](const PromiseError& e) mutable { reject(e); };
            if (currentState->state == PromiseState::Fulfilled) {
                handleFulfilled(currentState->value);
            } else if (currentState->state == PromiseState::Rejected) {
                handleRejected(currentState->error);
            } else {
                currentState->thenCallbacks.push_back(std::move(handleFulfilled));
                currentState->catchCallbacks.push_back(std::move(handleRejected));
            }
        });
        return next;
    }

    Promise& catchError(OnRejected onRejected) {
        if (d) {
            d->catchCallbacks.push_back(std::move(onRejected));
            d->maybeInvoke();
        }
        return *this;
    }


    PromiseState state() const { return d ? d->state : PromiseState::Pending; }
    bool isPending()   const { return state() == PromiseState::Pending; }
    bool isFulfilled() const { return state() == PromiseState::Fulfilled; }
    bool isRejected()  const { return state() == PromiseState::Rejected; }
    const T& value() const { return d->value; }
    const PromiseError& error() const { return d->error; }

    /// 创建已完成的 Promise（静态工厂，避免与实例 resolve(T) 重载冲突）
    static Promise resolved(T val) {
        Promise p([val](ResolveFn resolve, RejectFn) {
            resolve(val);
        });
        return p;
    }

    /// 创建已拒绝的 Promise（静态工厂，避免与实例 reject() 重载冲突）
    static Promise rejected(const PromiseError& err) {
        Promise p([err](ResolveFn, RejectFn reject) {
            reject(err);
        });
        return p;
    }

    static Promise rejected(const std::string& message, int code = 0) {
        return rejected(PromiseError(message, code));
    }


private:
    struct State {
        PromiseState state{PromiseState::Pending};
        T value{};
        PromiseError error;
        std::vector<OnFulfilled> thenCallbacks;
        std::vector<OnRejected>  catchCallbacks;

        void maybeInvoke() {
            if (state == PromiseState::Fulfilled) {
                for (auto& cb : thenCallbacks) { if (cb) cb(value); }
                thenCallbacks.clear();
                catchCallbacks.clear();
            } else if (state == PromiseState::Rejected) {
                for (auto& cb : catchCallbacks) { if (cb) cb(error); }
                thenCallbacks.clear();
                catchCallbacks.clear();
            }
        }
    };
    std::shared_ptr<State> d;
    ResolveFn m_resolveFn;  // 保存 resolve 函数（executor 构造时创建）
    RejectFn  m_rejectFn;   // 保存 reject 函数（executor 构造时创建）
};

// ---------- void 特化 ----------
template<>
class Promise<void> {
public:
    using OnFulfilled = std::function<void()>;
    using OnRejected  = std::function<void(const PromiseError&)>;
    using ResolveFn   = std::function<void()>;
    using RejectFn   = std::function<void(const PromiseError&)>;
    using Executor   = std::function<void(ResolveFn, RejectFn)>;

    Promise() = default;

    /// JavaScript 风格：new Promise((resolve, reject) => { ... })
    /// executor 必须提供，不能为空
    explicit Promise(Executor executor) {
        d = std::make_shared<State>();
        // Capture shared_ptr instead of this to ensure thread safety and lifetime safety
        auto statePtr = d;
        m_resolveFn = [statePtr] {
            if (!statePtr || statePtr->state != PromiseState::Pending) return;
            statePtr->state = PromiseState::Fulfilled;
            statePtr->maybeInvoke();
        };
        m_rejectFn = [statePtr](const PromiseError& err) {
            if (!statePtr || statePtr->state != PromiseState::Pending) return;
            statePtr->state = PromiseState::Rejected;
            statePtr->error = err;
            statePtr->maybeInvoke();
        };
        try {
            executor(m_resolveFn, m_rejectFn);
        } catch (const std::exception& e) {
            m_rejectFn(PromiseError::fromException(e));
        } catch (...) {
            m_rejectFn(PromiseError::unknownException());
        }
    }

    void resolve() {
        if (m_resolveFn) {
            m_resolveFn();
        }
    }

    void reject(const PromiseError& err) {
        if (m_rejectFn) {
            m_rejectFn(err);
        }
    }

    void reject(const std::string& message, int code = 0) {
        reject(PromiseError(message, code));
    }

    template<typename F, typename G = std::nullptr_t>
    auto then(F&& onFulfilled, G&& onRejected) -> Promise<std::conditional_t<
        PromiseTraits<std::invoke_result_t<F>>::isPromise,
        typename PromiseTraits<std::invoke_result_t<F>>::ValueType,
        std::decay_t<std::invoke_result_t<F>>>> {
        using R = std::invoke_result_t<F>;
        using U = std::conditional_t<
            PromiseTraits<R>::isPromise,
            typename PromiseTraits<R>::ValueType,
            std::decay_t<R>>;
        
        if (!d) {
            // 如果当前 Promise 没有状态，返回一个已 reject 的 Promise
            return Promise<U>([](typename Promise<U>::ResolveFn, typename Promise<U>::RejectFn reject) {
                reject(PromiseError("Promise has no state", -1));
            });
        }

        // 使用非空 executor 创建 Promise
        Promise<U> next([onFulfilled = std::forward<F>(onFulfilled), 
                         onRejected = std::forward<G>(onRejected),
                         currentState = d](typename Promise<U>::ResolveFn resolve, typename Promise<U>::RejectFn reject) mutable {
            // 创建处理回调
            auto handleFulfilled = [onFulfilled = std::forward<F>(onFulfilled), resolve, reject, currentState]() mutable {
                try {
                    if constexpr (std::is_same_v<R, void>) {
                        onFulfilled();
                        if constexpr (std::is_same_v<U, void>) {
                            resolve();
                        } else {
                            reject(PromiseError("Invalid return type", -1));
                        }
                    } else {
                        R result = onFulfilled();
                        if constexpr (PromiseTraits<R>::isPromise) {
                            // 循环检测：检查返回的 Promise 是否指向当前 Promise 的状态
                            if (result.d && result.d == currentState) {
                                reject(PromiseError("Chaining cycle detected for promise", -1));
                                return;
                            }
                            // Handle Promise<void> case: then callback takes no parameters
                            if constexpr (std::is_same_v<typename PromiseTraits<R>::ValueType, void>) {
                                 result.then([resolve]() mutable { resolve(); });
                             } else {
                                result.then([resolve](const U& u) mutable { resolve(u); });
                             }
                            result.catchError([reject](const PromiseError& e) mutable { reject(e); });
                        } else {
                            resolve(std::move(result));
                        }
                    }
                } catch (const std::exception& e) {
                    reject(PromiseError::fromException(e));
                } catch (...) {
                    reject(PromiseError::unknownException());
                }
            };

            auto handleRejected = [onRejected = std::forward<G>(onRejected), resolve, reject, currentState](const PromiseError& e) mutable {
                if constexpr (std::is_same_v<G, std::nullptr_t>) {
                    reject(e);
                } else {
                    try {
                        using R2 = std::invoke_result_t<G, const PromiseError&>;
                        if constexpr (std::is_same_v<R2, void>) {
                            onRejected(e);
                            if constexpr (std::is_same_v<U, void>) {
                                resolve();
                            } else {
                                reject(PromiseError("Invalid return type", -1));
                            }
                        } else {
                            R2 result = onRejected(e);
                            if constexpr (PromiseTraits<R2>::isPromise) {
                                if (result.d && result.d == currentState) {
                                    reject(PromiseError("Chaining cycle detected for promise", -1));
                                    return;
                                }
                                // Handle Promise<void> case: then callback takes no parameters
                                if constexpr (std::is_same_v<typename PromiseTraits<R2>::ValueType, void>) {
                                    result.then([resolve]() mutable { resolve(); });
                                } else {
                                    result.then([resolve](const U& u) mutable { resolve(u); });
                                }
                                result.catchError([reject](const PromiseError& err) mutable { reject(err); });
                            } else {
                                resolve(std::move(result));
                            }
                        }
                    } catch (const std::exception& ex) {
                        reject(PromiseError::fromException(ex));
                    } catch (...) {
                        reject(PromiseError::unknownException());
                    }
                }
            };

            // 根据当前 Promise 的状态决定执行方式
            if (currentState->state == PromiseState::Fulfilled) {
                // 当前 Promise 已完成，立即执行 onFulfilled
                handleFulfilled();
            } else if (currentState->state == PromiseState::Rejected) {
                // 当前 Promise 已失败，执行 onRejected
                handleRejected(currentState->error);
            } else {
                currentState->thenCallbacks.push_back(std::move(handleFulfilled));
                currentState->catchCallbacks.push_back(std::move(handleRejected));
            }
        });
        
        return next;
    }

    template<typename F>
    auto then(F&& onFulfilled) -> Promise<std::conditional_t<
        PromiseTraits<std::invoke_result_t<F>>::isPromise,
        typename PromiseTraits<std::invoke_result_t<F>>::ValueType,
        std::decay_t<std::invoke_result_t<F>>>> {
        return then(std::forward<F>(onFulfilled), static_cast<std::nullptr_t>(nullptr));
    }

    template<typename F>
    auto then(F&& onFulfilled, std::nullptr_t) -> Promise<std::conditional_t<
        PromiseTraits<std::invoke_result_t<F>>::isPromise,
        typename PromiseTraits<std::invoke_result_t<F>>::ValueType,
        std::decay_t<std::invoke_result_t<F>>>> {
        using R = std::invoke_result_t<F>;
        using U = std::conditional_t<
            PromiseTraits<R>::isPromise,
            typename PromiseTraits<R>::ValueType,
            std::decay_t<R>>;
        if (!d) {
            return Promise<U>([](typename Promise<U>::ResolveFn, typename Promise<U>::RejectFn reject) {
                reject(PromiseError("Promise has no state", -1));
            });
        }
        Promise<U> next([onFulfilled = std::forward<F>(onFulfilled), currentState = d](
            typename Promise<U>::ResolveFn resolve, typename Promise<U>::RejectFn reject) mutable {
            auto handleFulfilled = [onFulfilled = std::forward<F>(onFulfilled), resolve, reject, currentState]() mutable {
                try {
                    if constexpr (std::is_same_v<R, void>) {
                        onFulfilled();
                        if constexpr (std::is_same_v<U, void>) { resolve(); }
                        else { reject(PromiseError("Invalid return type", -1)); }
                    } else {
                        R result = onFulfilled();
                        if constexpr (PromiseTraits<R>::isPromise) {
                            if (result.d && result.d == currentState) {
                                reject(PromiseError("Chaining cycle detected for promise", -1));
                                return;
                            }
                            // Handle Promise<void> case: then callback takes no parameters
                            if constexpr (std::is_same_v<typename PromiseTraits<R>::ValueType, void>) {
                                 result.then([resolve]() mutable { resolve(); });
                             } else {
                                result.then([resolve](const U& u) mutable { resolve(u); });
                             }
                            result.catchError([reject](const PromiseError& e) mutable { reject(e); });
                        } else {
                            resolve(std::move(result));
                        }
                    }
                } catch (const std::exception& e) {
                    reject(PromiseError::fromException(e));
                } catch (...) {
                    reject(PromiseError::unknownException());
                }
            };
            auto handleRejected = [reject](const PromiseError& e) mutable { reject(e); };
            if (currentState->state == PromiseState::Fulfilled) {
                handleFulfilled();
            } else if (currentState->state == PromiseState::Rejected) {
                handleRejected(currentState->error);
            } else {
                currentState->thenCallbacks.push_back(std::move(handleFulfilled));
                currentState->catchCallbacks.push_back(std::move(handleRejected));
            }
        });
        return next;
    }

    Promise<void>& catchError(OnRejected onRejected) {
        if (d) {
            d->catchCallbacks.push_back(std::move(onRejected));
            d->maybeInvoke();
        }
        return *this;
    }



    PromiseState state() const { return d ? d->state : PromiseState::Pending; }
    bool isPending()   const { return state() == PromiseState::Pending; }
    bool isFulfilled() const { return state() == PromiseState::Fulfilled; }
    bool isRejected()  const { return state() == PromiseState::Rejected; }
    PromiseError error() const { return d ? d->error : PromiseError(); }

    /// 创建已完成的 Promise（静态工厂，避免与实例 resolve() 重载冲突）
    static Promise resolved() {
        Promise p([](ResolveFn resolve, RejectFn) {
            resolve();
        });
        return p;
    }

    /// 创建已拒绝的 Promise（静态工厂，避免与实例 reject() 重载冲突）
    static Promise rejected(const PromiseError& err) {
        Promise p([err](ResolveFn, RejectFn reject) {
            reject(err);
        });
        return p;
    }

    static Promise rejected(const std::string& message, int code = 0) {
        return rejected(PromiseError(message, code));
    }

private:
    struct State {
        PromiseState state{PromiseState::Pending};
        PromiseError error;
        std::vector<OnFulfilled> thenCallbacks;
        std::vector<OnRejected>  catchCallbacks;

        void maybeInvoke() {
            if (state == PromiseState::Fulfilled) {
                for (auto& cb : thenCallbacks) { if (cb) cb(); }
                thenCallbacks.clear();
                catchCallbacks.clear();
            } else if (state == PromiseState::Rejected) {
                for (auto& cb : catchCallbacks) { if (cb) cb(error); }
                thenCallbacks.clear();
                catchCallbacks.clear();
            }
        }
    };
    std::shared_ptr<State> d;
    ResolveFn m_resolveFn;  // 保存 resolve 函数（executor 构造时创建）
    RejectFn  m_rejectFn;   // 保存 reject 函数（executor 构造时创建）
};

} // namespace VoiceWave



