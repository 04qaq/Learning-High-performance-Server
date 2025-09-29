// --------------------------- config.h ---------------------------
#pragma once
// config.h
// 配置系统——ConfigVar 与 Config 管理
// 依赖：log.h (项目日志系统) 与 boost::lexical_cast（header-only）
#include "libs/log.h"
#include <exception>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <typeinfo>
#include <list>
#include <algorithm>
#include <boost/lexical_cast.hpp>
#include <yaml-cpp/node/node.h>
#include <yaml-cpp/node/parse.h>
#include <yaml-cpp/yaml.h>

namespace sunshine {

// --------------------------- ConfigVarBase ---------------------------
// 基类：所有配置项的父类，包含名称、描述与纯虚序列化接口
class ConfigVarBase {
public:
    using ptr = std::shared_ptr<ConfigVarBase>;

    ConfigVarBase(std::string name, std::string description) :
        m_name(std::move(name)), m_description(std::move(description)) {
    }

    virtual ~ConfigVarBase() = default;

    const std::string &getName() const {
        return m_name;
    }
    const std::string &getDescription() const {
        return m_description;
    }

    // 把值序列化成字符串；从字符串反序列化到值
    virtual std::string toString() = 0;
    virtual bool fromString(const std::string &val) = 0;

protected:
    std::string m_name;
    std::string m_description;
};

// --------------------------- LexicalCast ---------------------------
// 通用的类型转换小工具，默认使用 boost::lexical_cast
template <class F, class T>
class LexicalCast {
public:
    T operator()(const F &val) {
        return boost::lexical_cast<T>(val);
    }
};

// string -> vector<T> 的特化（YAML 格式的字符串）
template <class T>
class LexicalCast<std::string, std::vector<T>> {
public:
    std::vector<T> operator()(const std::string &val) {
        std::vector<T> vec;
        try {
            YAML::Node node = YAML::Load(val);
            if (!node.IsSequence()) {
                if (node.IsScalar()) {
                    vec.push_back(LexicalCast<std::string, T>()(node.Scalar()));
                }
                return vec;
            }
            for (const auto &n : node) {
                std::string item_str;
                if (n.IsScalar()) {
                    item_str = n.Scalar();
                } else {
                    item_str = YAML::Dump(n);
                }
                vec.push_back(LexicalCast<std::string, T>()(item_str));
            }
        } catch (const std::exception &e) {
            LOG_ERROR(LogManager::GetInstance().getRoot()) << "LexicalCast<string, vector<T>> exception: " << e.what();
        } catch (...) {
            LOG_ERROR(LogManager::GetInstance().getRoot()) << "LexicalCast<string, vector<T>> unknown exception";
        }
        return vec;
    }
};

// vector<T> -> string 的特化（输出 YAML 序列文本）
template <class T>
class LexicalCast<std::vector<T>, std::string> {
public:
    std::string operator()(const std::vector<T> &val) {
        YAML::Node node(YAML::NodeType::Sequence);
        for (const auto &it : val) {
            std::string s = LexicalCast<T, std::string>()(it);
            try {
                YAML::Node n = YAML::Load(s);
                node.push_back(n);
            } catch (...) {
                node.push_back(s);
            }
        }
        return YAML::Dump(node);
    }
};

// --------------------------- ConfigVar<T> ---------------------------
// 将 ConfigVar 改造成线程安全：使用 shared_mutex 实现读多写少的场景
// - m_val: 由 m_mutex 保护，读时用 shared_lock，写时用 unique_lock
// - m_cb: 监听器 map 也由同一把 mutex 保护，写时复制回调列表到本地后在锁外执行回调
// 这样保证高并发读取时性能，同时安全地执行变更回调
template <class T, class FromStr = LexicalCast<std::string, T>, class ToStr = LexicalCast<T, std::string>>
class ConfigVar : public ConfigVarBase {
public:
    using ptr = std::shared_ptr<ConfigVar<T>>;
    typedef std::function<void(const T &old_val, const T &new_val)> on_change_cb;

    ConfigVar(const std::string &name, const T &val, const std::string &description = "") :
        ConfigVarBase(name, description), m_val(val) {
    }

    std::string toString() override {
        try {
            std::shared_lock<std::shared_mutex> sl(m_mutex);
            return ToStr()(m_val);
        } catch (const std::exception &e) {
            LOG_ERROR(LogManager::GetInstance().getRoot())
                << "ConfigVar::toString exception: " << e.what() << " type: " << typeid(m_val).name();
        } catch (...) {
            LOG_ERROR(LogManager::GetInstance().getRoot())
                << "ConfigVar::toString unknown exception, type: " << typeid(m_val).name();
        }
        return std::string();
    }

    bool fromString(const std::string &val) override {
        try {
            T tmp = FromStr()(val);
            setValue(tmp);
            return true;
        } catch (const std::exception &e) {
            LOG_ERROR(LogManager::GetInstance().getRoot())
                << "ConfigVar::fromString exception: " << e.what() << " type: " << typeid(m_val).name();
        } catch (...) {
            LOG_ERROR(LogManager::GetInstance().getRoot())
                << "ConfigVar::fromString unknown exception, type: " << typeid(m_val).name();
        }
        return false;
    }

    // 值访问（线程安全）
    T getValue() const {
        std::shared_lock<std::shared_mutex> sl(m_mutex);
        return m_val;
    }

    void setValue(const T &v) {
        // 采用写锁进行修改并安全地通知回调：
        // 1) 获取 unique_lock，比较并更新 m_val
        // 2) 在锁内拷贝回调列表到局部容器
        // 3) 释放锁，逐个调用回调（避免在持锁期间调用用户代码）
        std::unique_lock<std::shared_mutex> ul(m_mutex);
        if (v == m_val) return;
        T old = m_val;
        m_val = v;
        // 复制回调到本地列表
        std::vector<on_change_cb> cbs;
        cbs.reserve(m_cb.size());
        for (auto &kv : m_cb) {
            if (kv.second) cbs.push_back(kv.second);
        }
        ul.unlock();

        // 在锁外执行回调，避免回调中再尝试获取锁导致死锁
        for (auto &cb : cbs) {
            try {
                cb(old, v);
            } catch (const std::exception &e) {
                LOG_ERROR(LogManager::GetInstance().getRoot()) << "ConfigVar callback exception: " << e.what();
            } catch (...) {
                LOG_ERROR(LogManager::GetInstance().getRoot()) << "ConfigVar callback unknown exception";
            }
        }
    }

    void addListener(uint64_t key, on_change_cb cb) {
        std::unique_lock<std::shared_mutex> ul(m_mutex);
        m_cb[key] = std::move(cb);
    }

    void delListener(uint64_t key) {
        std::unique_lock<std::shared_mutex> ul(m_mutex);
        m_cb.erase(key);
    }

    on_change_cb getListener(uint64_t key) {
        std::shared_lock<std::shared_mutex> sl(m_mutex);
        auto it = m_cb.find(key);
        if (it == m_cb.end()) return on_change_cb();
        return it->second;
    }

private:
    mutable std::shared_mutex m_mutex; // 保护 m_val 与 m_cb
    T m_val;
    std::map<uint64_t, on_change_cb> m_cb;
};

// --------------------------- Config 管理器 ---------------------------
class Config {
public:
    using ConfigMap = std::map<std::string, ConfigVarBase::ptr>;

    static ConfigVarBase::ptr LookupBase(const std::string &name);

    template <class T>
    static typename ConfigVar<T>::ptr Lookup(const std::string &name,
                                             const T &default_val,
                                             const std::string &description = "") {
        if (!ValidName(name)) {
            LOG_ERROR(LogManager::GetInstance().getRoot()) << "Config::Lookup invalid name: " << name;
            throw std::invalid_argument("invalid config name: " + name);
        }

        // 读多写少：先用 shared_lock 检查是否存在，若不存在再升级为 unique_lock 创建
        {
            std::shared_lock<std::shared_mutex> sl(s_mutex);
            auto it = s_datas.find(name);
            if (it != s_datas.end()) {
                auto tmp = std::dynamic_pointer_cast<ConfigVar<T>>(it->second);
                if (tmp) {
                    LOG_INFO(LogManager::GetInstance().getRoot()) << "Config::Lookup name = " << name << " exists, return existing";
                    return tmp;
                } else {
                    LOG_ERROR(LogManager::GetInstance().getRoot()) << "Config::Lookup name = " << name << " exists but type mismatch";
                    return nullptr;
                }
            }
        }

        // 升级到写锁创建
        std::unique_lock<std::shared_mutex> ul(s_mutex);
        auto it2 = s_datas.find(name);
        if (it2 != s_datas.end()) {
            return std::dynamic_pointer_cast<ConfigVar<T>>(it2->second);
        }

        auto v = std::make_shared<ConfigVar<T>>(name, default_val, description);
        s_datas[name] = v;
        LOG_INFO(LogManager::GetInstance().getRoot()) << "Config::Lookup created config name = " << name;
        return v;
    }

    template <class T>
    static typename ConfigVar<T>::ptr Lookup(const std::string &name) {
        std::shared_lock<std::shared_mutex> sl(s_mutex);
        auto it = s_datas.find(name);
        if (it == s_datas.end()) return nullptr;
        return std::dynamic_pointer_cast<ConfigVar<T>>(it->second);
    }

    static ConfigMap getAll() {
        std::shared_lock<std::shared_mutex> sl(s_mutex);
        return s_datas;
    }

    static void ListAllMember(const std::string &prefix, const YAML::Node &node,
                              std::list<std::pair<std::string, YAML::Node>> &output);

    static void FromYaml(const YAML::Node &root);

private:
    static bool ValidName(const std::string &s) {
        if (s.empty()) return false;
        for (unsigned char uc : s) {
            if ((uc >= '0' && uc <= '9') || (uc >= 'A' && uc <= 'Z') || (uc >= 'a' && uc <= 'z') || uc == '.' || uc == '_') {
                continue;
            }
            return false;
        }
        return true;
    }

private:
    inline static ConfigMap s_datas;
    // s_mutex 改为 shared_mutex 以优化读多写少场景
    inline static std::shared_mutex s_mutex;
};

} // namespace sunshine
