#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace tr {

struct Section {
    uint32_t rva;
    uint32_t size;
    bool exec;
};

struct Module {
    uint8_t* base = nullptr;
    uint32_t image_size = 0;
    std::vector<Section> sections;
    uint32_t exc_rva = 0, exc_size = 0;  // .pdata, used by function_start

    bool init(void* hmodule);
    bool contains(const void* p, size_t n = 1) const;
    bool in_exec(const void* p) const;
    uint32_t rva(const void* p) const { return uint32_t(static_cast<const uint8_t*>(p) - base); }
    const uint8_t* function_start(const void* p) const;  // follows chained unwind info back to the real function start
};

struct VfTable {
    void** vft;
    uint32_t offset;
    uint32_t slots;
};

class RttiIndex {
public:
    void build(const Module& m);
    const std::vector<VfTable>* find(const std::string& mangled) const;
    void** primary(const std::string& mangled) const;
    std::string class_of(const void* obj, uint32_t* subobject_offset = nullptr) const;
    size_t size() const { return by_name_.size(); }

private:
    const Module* m_ = nullptr;
    std::unordered_map<std::string, std::vector<VfTable>> by_name_;
};

}  // namespace tr
