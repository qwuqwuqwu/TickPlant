#pragma once

// data_source_registry.hpp
//
// Central registry for DataSource implementations.
//
// Usage:
//   DataSourceRegistry reg;
//   reg.register_source(std::make_unique<ExchangeDataSource>(engine, "Binance"));
//   reg.register_source(std::make_unique<QuestDbDataSource>());
//
//   DataSource* src = reg.get("Binance");   // never nullptr if registered
//
// The registry owns all sources via unique_ptr.
// Not thread-safe for registration — call register_source() before start().

#include "data_source.hpp"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class DataSourceRegistry {
public:
    // Register a source. Overwrites any previous entry with the same name.
    void register_source(std::unique_ptr<DataSource> src);

    // Look up a source by name. Returns nullptr if not found.
    DataSource* get(const std::string& name) const;

    // All registered source names (order unspecified).
    std::vector<std::string> names() const;

private:
    std::unordered_map<std::string, std::unique_ptr<DataSource>> sources_;
};
