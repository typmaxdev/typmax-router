// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 The typmax-router authors
// Stands in for KiCad's include/settings/parameters.h: the PARAM kinds
// ROUTING_SETTINGS registers (see settings/nested_settings.h).
#pragma once

#include <functional>
#include <string>
#include <type_traits>

#include <settings/nested_settings.h>

template <typename T>
class PARAM : public PARAM_BASE {
public:
    PARAM(const std::string& aPath, T* aPtr, T aDefault) : PARAM_BASE(aPath), m_ptr(aPtr) { (void) aDefault; }
    bool SetInt(int v) override { return assign(v); }
    bool SetDouble(double v) override { return assign(v); }
    bool SetBool(bool v) override { return assign(v); }

private:
    template <typename V>
    bool assign(V v)
    {
        if constexpr (std::is_same_v<V, T>) {
            *m_ptr = v;
            return true;
        }
        return false;
    }
    T* m_ptr;
};

template <typename T>
class PARAM_LAMBDA : public PARAM_BASE {
public:
    PARAM_LAMBDA(const std::string& aPath, std::function<T()> aGetter, std::function<void(T)> aSetter, T aDefault) :
            PARAM_BASE(aPath), m_setter(std::move(aSetter))
    {
        (void) aGetter;
        (void) aDefault;
    }
    bool SetInt(int v) override { return assign(v); }
    bool SetDouble(double v) override { return assign(v); }
    bool SetBool(bool v) override { return assign(v); }

private:
    template <typename V>
    bool assign(V v)
    {
        if constexpr (std::is_same_v<V, T>) {
            m_setter(v);
            return true;
        }
        return false;
    }
    std::function<void(T)> m_setter;
};

template <typename T>
class PARAM_ENUM : public PARAM_BASE {
public:
    PARAM_ENUM(const std::string& aPath, T* aPtr, T aDefault, T aMin, T aMax) : PARAM_BASE(aPath), m_ptr(aPtr)
    {
        (void) aDefault;
        (void) aMin;
        (void) aMax;
    }
    bool SetInt(int v) override
    {
        *m_ptr = static_cast<T>(v);
        return true;
    }

private:
    T* m_ptr;
};
