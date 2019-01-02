////////////////////////////////////////////////////////////////////////////////
// Copyright (c) 2016, Andrew Dornbush
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//     1. Redistributions of source code must retain the above copyright notice
//        this list of conditions and the following disclaimer.
//     2. Redistributions in binary form must reproduce the above copyright
//        notice, this list of conditions and the following disclaimer in the
//        documentation and/or other materials provided with the distribution.
//     3. Neither the name of the copyright holder nor the names of its
//        contributors may be used to endorse or promote products derived from
//        this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
////////////////////////////////////////////////////////////////////////////////

/// \author Andrew Dornbush

#ifndef SMPL_EXTENSION_H
#define SMPL_EXTENSION_H

// standard includes
#include <stddef.h>
#include <typeinfo>

namespace smpl {

template <typename T>
size_t GetClassCode()
{
    return typeid(T).hash_code();
}

class Extension
{
public:

    virtual ~Extension() { }

    template <typename T>
    T* GetExtension()
    {
        auto* e = GetExtension(GetClassCode<T>());
        return dynamic_cast<T*>(e);
    }

    virtual Extension* GetExtension(size_t class_code) = 0;
};

template <class ExtensionType, class ParentType = Extension>
struct ChildInterface : public ExtensionType
{
    // TODO: static assert ParentType is an Extension

    ParentType* parent;

    ChildInterface(ParentType* parent) : parent(parent) { }

    auto GetExtension(size_t class_code) -> Extension* final
    {
        if (class_code == GetClassCode<ExtensionType>()) {
            return this;
        }
        return parent->GetExtension(class_code);
    }
};

} // namespace smpl

#endif
