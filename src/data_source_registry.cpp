#include "data_source_registry.hpp"

void DataSourceRegistry::register_source(std::unique_ptr<DataSource> src) {
    if (!src) return;
    const std::string key = src->name();
    sources_[key] = std::move(src);
}

DataSource* DataSourceRegistry::get(const std::string& n) const {
    auto it = sources_.find(n);
    return (it != sources_.end()) ? it->second.get() : nullptr;
}

std::vector<std::string> DataSourceRegistry::names() const {
    std::vector<std::string> result;
    result.reserve(sources_.size());
    for (const auto& [k, _] : sources_) result.push_back(k);
    return result;
}
