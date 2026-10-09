// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 The typmax-router authors
// Stands in for KiCad's JSON settings (include/settings/json_settings.h,
// nested_settings.h, parameters.h): ROUTING_SETTINGS registers its persistent
// parameters as PARAMs; here they live in memory and can be set by path, the
// way KiCad loads them from its settings file. Set("shove_time_limit", …)
// reaches the private shove time limit through its own PARAM_LAMBDA.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

class PARAM_BASE {
public:
    explicit PARAM_BASE(std::string aPath) : m_path(std::move(aPath)) {}
    virtual ~PARAM_BASE() = default;
    const std::string& GetJsonPath() const { return m_path; }
    virtual bool SetInt(int) { return false; }
    virtual bool SetDouble(double) { return false; }
    virtual bool SetBool(bool) { return false; }

private:
    std::string m_path;
};

class JSON_SETTINGS {
public:
    virtual ~JSON_SETTINGS() = default;
    void LoadFromFile() {}

    // false when no parameter has that path or it takes another type
    template <typename T>
    bool Set(const std::string& aPath, T aVal)
    {
        for (auto& p : m_params) {
            if (p->GetJsonPath() != aPath)
                continue;
            if constexpr (std::is_same_v<T, bool>)
                return p->SetBool(aVal);
            else if constexpr (std::is_integral_v<T>)
                return p->SetInt(static_cast<int>(aVal));
            else
                return p->SetDouble(static_cast<double>(aVal));
        }
        return false;
    }

protected:
    std::vector<std::unique_ptr<PARAM_BASE>> m_params;
};

class NESTED_SETTINGS : public JSON_SETTINGS {
public:
    NESTED_SETTINGS(const std::string&, int, JSON_SETTINGS*, const std::string&) {}
};
