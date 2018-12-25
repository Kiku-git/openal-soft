#ifndef BACKENDS_QSA_H
#define BACKENDS_QSA_H

#include "backends/base.h"

struct QSABackendFactory final : public BackendFactory {
public:
    bool init() override;
    void deinit() override;

    bool querySupport(ALCbackend_Type type) override;

    void probe(DevProbe type, std::string *outnames) override;

    ALCbackend *createBackend(ALCdevice *device, ALCbackend_Type type) override;

    static BackendFactory &getFactory();
};

#endif /* BACKENDS_QSA_H */
