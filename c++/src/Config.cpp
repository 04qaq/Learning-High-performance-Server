// --------------------------- config.cpp ---------------------------
#include "libs/log.h"
#include <libs/Config.h>
#include <ostream>

namespace sunshine {

// 返回基础指针（线程安全读取）
ConfigVarBase::ptr Config::LookupBase(const std::string &name) {
    std::shared_lock<std::shared_mutex> sl(s_mutex);
    auto it = s_datas.find(name);
    if (it == s_datas.end())
        return nullptr;
    return it->second;
}

// 递归列出 YAML 所有成员（将 nested map 转为 dotted keys）
void Config::ListAllMember(const std::string &prefix, const YAML::Node &node,
                           std::list<std::pair<std::string, YAML::Node>> &output) {
    if (!ValidName(prefix) && !prefix.empty()) {
        LOG_ERROR(LogManager::GetInstance().getRoot()) << "Config invalid name " << prefix << " : " << node;
        return;
    }

    // 把当前节点加入输出（注意：prefix 为空允许）
    output.push_back({prefix, node});

    if (node.IsMap()) {
        for (auto it = node.begin(); it != node.end(); ++it) {
            std::string key = it->first.Scalar();
            std::string new_prefix = prefix.empty() ? key : (prefix + "." + key);
            ListAllMember(new_prefix, it->second, output);
        }
    }
}

// 根据 YAML 根节点把值写入已注册的 ConfigVar（如果存在）
void Config::FromYaml(const YAML::Node &root) {
    std::list<std::pair<std::string, YAML::Node>> all;
    ListAllMember("", root, all);

    for (auto &kv : all) {
        const std::string &key = kv.first;
        const YAML::Node &node = kv.second;
        if (key.empty()) continue;
        auto base = LookupBase(key);
        if (!base) continue;

        if (node.IsScalar()) {
            base->fromString(node.Scalar());
        } else {
            std::stringstream ss;
            ss << node;
            base->fromString(ss.str());
        }
    }
}

} // namespace sunshine