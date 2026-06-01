// var_attributes.cpp -- implementation of the CF/UGRID attribute model.
//
// See var_attributes.hpp for the contract.  The manifest parsing half
// only compiles meaningfully when eckit is in the build (it relies on
// eckit::Configuration::keys() / getSubConfiguration()).  In non-eckit
// builds the parser degrades to "defaults only", which is sufficient
// for the lightweight test configurations that do not exercise eckit.

#include "drivers/common/var_attributes.hpp"

#include <cctype>
#include <cstdlib>

#ifdef AMIO_HAS_ECKIT
#include <eckit/config/Configuration.h>
#include <eckit/config/LocalConfiguration.h>
#endif

namespace amio::detail {

// ---------------------------------------------------------------
// parse_attr_value -- detect numeric literals; keep original text.
// ---------------------------------------------------------------

AttrValue parse_attr_value(const std::string& raw) {
    AttrValue v;
    v.text = raw;
    v.is_numeric = false;
    v.is_integer = false;

    if (raw.empty()) {
        return v;
    }

    // Attempt a full-string numeric parse via std::strtod.  Only treat
    // the value as numeric when the entire (trimmed) string is consumed.
    const char* begin = raw.c_str();
    char* end = nullptr;
    errno = 0;
    double d = std::strtod(begin, &end);

    // Skip trailing whitespace after the parsed number.
    while (end != nullptr && *end != '\0' && std::isspace(static_cast<unsigned char>(*end))) {
        ++end;
    }

    if (end != nullptr && *end == '\0' && end != begin) {
        v.is_numeric = true;
        v.number = d;
        // Integer iff no decimal point / exponent appears in the text.
        v.is_integer = raw.find('.') == std::string::npos && raw.find('e') == std::string::npos && raw.find('E') == std::string::npos;
    }

    return v;
}

#ifdef AMIO_HAS_ECKIT

namespace {

// Copy every scalar key under `cfg` into `out` as an AttrValue.
void read_attr_block(const eckit::Configuration& cfg, VarAttributes& out) {
    for (const std::string& key : cfg.keys()) {
        // Only scalar leaves are attributes; skip nested maps.
        std::string raw;
        try {
            raw = cfg.getString(key);
        } catch (...) {
            // Non-scalar (sub-map / list) -- not a simple attribute.
            continue;
        }
        out.set(key, parse_attr_value(raw));
    }
}

// Detect a UGRID role in a variable's attribute set.
bool has_ugrid_role(const VarAttributes& attrs) {
    for (const auto& kv : attrs.items) {
        if (kv.first == "cf_role" || kv.first == "topology_dimension" || kv.first == "mesh_topology") {
            return true;
        }
    }
    return false;
}

}  // namespace

DatasetAttributes parse_dataset_attributes(const eckit::Configuration& config) {
    DatasetAttributes out;

    // Global extra attributes.
    if (config.has("global_attributes")) {
        try {
            read_attr_block(config.getSubConfiguration("global_attributes"), out.global);
        } catch (...) {
            // Malformed block -- ignore; globals stay empty.
        }
    }

    // Per-variable attributes.
    if (config.has("variables")) {
        try {
            eckit::LocalConfiguration vars = config.getSubConfiguration("variables");
            for (const std::string& var_name : vars.keys()) {
                eckit::LocalConfiguration var_cfg = vars.getSubConfiguration(var_name);
                if (!var_cfg.has("attributes")) {
                    continue;
                }
                VarAttributes attrs;
                read_attr_block(var_cfg.getSubConfiguration("attributes"), attrs);
                if (has_ugrid_role(attrs)) {
                    out.uses_ugrid = true;
                }
                out.per_variable.emplace(var_name, std::move(attrs));
            }
        } catch (...) {
            // Malformed block -- ignore; per-variable map stays as-is.
        }
    }

    // Resolve the Conventions string: explicit manifest value wins;
    // otherwise default to CF, upgraded to CF+UGRID when a mesh role
    // was declared.
    if (config.has("conventions")) {
        out.conventions = config.getString("conventions", kDefaultCFConventions);
    } else {
        out.conventions = out.uses_ugrid ? kDefaultCFUGRIDConventions : kDefaultCFConventions;
    }

    return out;
}

#else  // !AMIO_HAS_ECKIT

DatasetAttributes parse_dataset_attributes(const eckit::Configuration& /*config*/) {
    // Without eckit there is no keys()/getSubConfiguration() to walk;
    // return CF defaults so drivers still emit a Conventions attribute.
    return DatasetAttributes{};
}

#endif  // AMIO_HAS_ECKIT

}  // namespace amio::detail
