#pragma once

#include <jsi/jsi.h>

struct napi_env__ {
    facebook::jsi::Runtime* context{};

    napi_env__(facebook::jsi::Runtime* context) : context{context} {
    }

    ~napi_env__() {
    }
};