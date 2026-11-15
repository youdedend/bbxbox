// SPDX-License-Identifier: GPL-3.0-or-later
// A stub of <wrl/client.h>'s ComPtr, for tools/syntax_check.sh only.
// The member set mirrors the real class closely enough that a translation unit
// which compiles here compiles against the SDK: Get, GetAddressOf,
// ReleaseAndGetAddressOf, As, Put, Reset, Attach, Detach, operator-> and the
// bool conversion.
#pragma once

#include <utility>

#include <windows.h>

namespace Microsoft::WRL {

template <class T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(std::nullptr_t) {}
    template <class U>
    ComPtr(U* other) : ptr_(other) {
        if (ptr_) {
            ptr_->AddRef();
        }
    }
    ComPtr(const ComPtr& other) : ptr_(other.ptr_) {
        if (ptr_) {
            ptr_->AddRef();
        }
    }
    ComPtr(ComPtr&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }
    template <class U>
    ComPtr(ComPtr<U>&& other) noexcept : ptr_(static_cast<T*>(other.Detach())) {}
    ~ComPtr() { Reset(); }

    ComPtr& operator=(const ComPtr& other) {
        if (this != &other) {
            ComPtr copy(other);
            Swap(copy);
        }
        return *this;
    }
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            Reset();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }
    ComPtr& operator=(std::nullptr_t) {
        Reset();
        return *this;
    }

    [[nodiscard]] T* Get() const { return ptr_; }
    [[nodiscard]] T** GetAddressOf() { return &ptr_; }
    [[nodiscard]] T* const* GetAddressOf() const { return &ptr_; }
    [[nodiscard]] T** ReleaseAndGetAddressOf() {
        Reset();
        return &ptr_;
    }
    [[nodiscard]] T* Detach() {
        T* const out = ptr_;
        ptr_ = nullptr;
        return out;
    }
    void Attach(T* other) {
        Reset();
        ptr_ = other;
    }
    void Reset() {
        if (ptr_) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }
    void Swap(ComPtr& other) noexcept { std::swap(ptr_, other.ptr_); }

    template <class U>
    HRESULT As(ComPtr<U>* out) const {
        return ptr_ ? ptr_->QueryInterface(__uuidof(U), reinterpret_cast<void**>(out->GetAddressOf()))
                    : E_NOINTERFACE;
    }
    template <class U>
    HRESULT As(U** out) const {
        return ptr_ ? ptr_->QueryInterface(__uuidof(U), reinterpret_cast<void**>(out)) : E_NOINTERFACE;
    }
    void Put() const {}

    [[nodiscard]] T* operator->() const { return ptr_; }
    [[nodiscard]] explicit operator bool() const { return ptr_ != nullptr; }
    [[nodiscard]] bool operator==(std::nullptr_t) const { return ptr_ == nullptr; }
    [[nodiscard]] bool operator!=(std::nullptr_t) const { return ptr_ != nullptr; }

private:
    T* ptr_ = nullptr;
};

}  // namespace Microsoft::WRL
