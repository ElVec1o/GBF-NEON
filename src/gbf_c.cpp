// GBF C API implementation — thin wrapper around GaloisBipartiteFilterImpl<2>.

#include "gbf_c.h"
#include "gbf.hpp"

#include <exception>
#include <new>
#include <vector>

using Impl = gbf::GaloisBipartiteFilterImpl<2>;

static inline Impl* as_impl(gbf_filter_t* f) noexcept {
    return f ? static_cast<Impl*>(f->opaque) : nullptr;
}
static inline const Impl* as_impl(const gbf_filter_t* f) noexcept {
    return f ? static_cast<const Impl*>(f->opaque) : nullptr;
}

extern "C" {

bool gbf_filter_allocate(uint32_t /*n_keys*/, gbf_filter_t* f) {
    if (!f) return false;
    // GBF is built-once from a key list, not incrementally — so allocate
    // here just resets to "empty, not-yet-populated" state. populate() does
    // the actual sizing + build.
    if (f->opaque) {
        delete static_cast<Impl*>(f->opaque);
    }
    f->opaque = nullptr;
    return true;
}

bool gbf_filter_populate(const uint64_t* keys, uint32_t n_keys, gbf_filter_t* f) {
    if (!f) return false;
    if (f->opaque) {
        delete static_cast<Impl*>(f->opaque);
        f->opaque = nullptr;
    }
    if (!keys && n_keys != 0) return false;
    try {
        std::vector<uint64_t> kv(keys, keys + n_keys);
        Impl* p = new (std::nothrow) Impl(kv, /*threads=*/0);
        if (!p) return false;
        f->opaque = p;
        return true;
    } catch (const std::exception&) {
        return false;
    } catch (...) {
        return false;
    }
}

bool gbf_filter_contain(uint64_t key, const gbf_filter_t* f) {
    const Impl* p = as_impl(f);
    if (!p) return false;
    return p->contains(key);
}

void gbf_filter_free(gbf_filter_t* f) {
    if (!f) return;
    delete static_cast<Impl*>(f->opaque);
    f->opaque = nullptr;
}

size_t gbf_filter_size_bytes(const gbf_filter_t* f) {
    const Impl* p = as_impl(f);
    if (!p) return 0;
    return p->memory_bytes();
}

bool gbf_filter_save(const gbf_filter_t* f, const char* path) {
    const Impl* p = as_impl(f);
    if (!p || !path) return false;
    try {
        return p->save(std::string(path));
    } catch (...) {
        return false;
    }
}

bool gbf_filter_load(gbf_filter_t* f, const char* path) {
    if (!f || !path) return false;
    if (f->opaque) {
        delete static_cast<Impl*>(f->opaque);
        f->opaque = nullptr;
    }
    try {
        Impl tmp = Impl::load(std::string(path));
        Impl* p = new (std::nothrow) Impl(std::move(tmp));
        if (!p) return false;
        f->opaque = p;
        return true;
    } catch (...) {
        return false;
    }
}

} // extern "C"
