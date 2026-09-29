#pragma once

#include <worldanalysis/sdk/Memory.hpp>
#include <worldanalysis/sdk/Offsets.hpp>
#include <string>

namespace worldanalysis::sdk {

class Block {
public:
    void* blockType() const { return field<void*>(this, offsets::Block::mBlockType); }

    const std::string* fullName() const {
        auto type = reinterpret_cast<std::uintptr_t>(blockType());
        if (!type) return nullptr;
        auto hashed = type + offsets::BlockType::mNameInfo + offsets::NameInfo::mFullName;
        return reinterpret_cast<const std::string*>(hashed + offsets::HashedString::mString);
    }
};

}
