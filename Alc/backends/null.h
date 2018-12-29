#ifndef BACKENDS_NULL_H
#define BACKENDS_NULL_H

#include "backends/base.h"

struct NullBackendFactory final : public BackendFactory {
public:
    bool init() override;
    /*void deinit() override;*/

    bool querySupport(ALCbackend_Type type) override;

    void probe(DevProbe type, std::string *outnames) override;

    BackendBase *createBackend(ALCdevice *device, ALCbackend_Type type) override;

    static BackendFactory &getFactory();
};

#endif /* BACKENDS_NULL_H */
