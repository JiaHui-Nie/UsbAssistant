#pragma once

// 极简 COM 智能指针，避免依赖 ATL / WRL。
template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    explicit ComPtr(T* p) : p_(p) {}
    ComPtr(const ComPtr& other) : p_(other.p_) {
        if (p_) p_->AddRef();
    }
    ComPtr(ComPtr&& other) noexcept : p_(other.p_) { other.p_ = nullptr; }
    ~ComPtr() { Reset(); }

    ComPtr& operator=(const ComPtr& other) {
        if (this != &other) {
            Reset();
            p_ = other.p_;
            if (p_) p_->AddRef();
        }
        return *this;
    }
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            Reset();
            p_ = other.p_;
            other.p_ = nullptr;
        }
        return *this;
    }

    void Reset() {
        if (p_) {
            p_->Release();
            p_ = nullptr;
        }
    }

    T* Get() const { return p_; }
    T** Put() {
        Reset();
        return &p_;
    }
    T* operator->() const { return p_; }
    operator T*() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

private:
    T* p_ = nullptr;
};
