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
#include <typeinfo>
#include <list>
#include <boost/lexical_cast.hpp>
#include <yaml-cpp/node/node.h>
#include <yaml-cpp/node/parse.h>
#include <yaml-cpp/yaml.h>

namespace Log {

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
            m_val = FromStr()(val);
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

    // 值访问（注意：若多线程读写，请加锁）
    T getValue() const {
        return m_val;
    }
    void setValue(const T &v) {
        if (v == m_val) return;
        for (auto it : m_cb) {
            it(m_val, v);
        }
        m_val = v;
    }
    void addListener(uint64_t key, on_change_cb cb) {
        m_cb[key] = cb;
    }
    void delListener(uint64_t key) {
        m_cb.erase(key);
    }
    on_change_cb getListener(uint64_t key) {
        return m_cb[key];
    }

private:
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

        std::lock_guard<std::mutex> lk(s_mutex);
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

        auto v = std::make_shared<ConfigVar<T>>(name, default_val, description);
        s_datas[name] = v;
        LOG_INFO(LogManager::GetInstance().getRoot()) << "Config::Lookup created config name = " << name;
        return v;
    }

    template <class T>
    static typename ConfigVar<T>::ptr Lookup(const std::string &name) {
        std::lock_guard<std::mutex> lk(s_mutex);
        auto it = s_datas.find(name);
        if (it == s_datas.end()) return nullptr;
        return std::dynamic_pointer_cast<ConfigVar<T>>(it->second);
    }

    static ConfigMap getAll() {
        std::lock_guard<std::mutex> lk(s_mutex);
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
    inline static std::mutex s_mutex;
};

} // namespace Log